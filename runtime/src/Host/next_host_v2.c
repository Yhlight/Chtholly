#include "chtholly/next_host_v2.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET v2_socket;
#define V2_INVALID_SOCKET INVALID_SOCKET
#define v2_close_socket closesocket
#define v2_last_error WSAGetLastError()
#define V2_EINTR WSAEINTR
#define V2_EWOULDBLOCK WSAEWOULDBLOCK
#define V2_EINPROGRESS WSAEINPROGRESS
#define V2_ECONNRESET WSAECONNRESET
#define V2_SHUT_RD SD_RECEIVE
#define V2_SHUT_WR SD_SEND
#define V2_SHUT_RDWR SD_BOTH
static int v2_startup(void) {
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
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int v2_socket;
#define V2_INVALID_SOCKET (-1)
#define v2_close_socket close
#define v2_last_error errno
#define V2_EINTR EINTR
#define V2_EWOULDBLOCK EWOULDBLOCK
#define V2_EINPROGRESS EINPROGRESS
#define V2_ECONNRESET ECONNRESET
#define V2_SHUT_RD SHUT_RD
#define V2_SHUT_WR SHUT_WR
#define V2_SHUT_RDWR SHUT_RDWR
static int v2_startup(void) { return 0; }
#endif

typedef struct V2Handle {
  v2_socket fd;
  uint32_t magic;
} V2Handle;

#define V2_MAGIC 0x5632534fu

static V2Handle *v2_handle(void *opaque) {
  V2Handle *handle = (V2Handle *)opaque;
  return handle != NULL && handle->magic == V2_MAGIC ? handle : NULL;
}

static int v2_wait(v2_socket socket, int write, uint64_t timeout_ms) {
#if defined(_WIN32)
  fd_set set;
  struct timeval timeout;
  FD_ZERO(&set);
  FD_SET(socket, &set);
  timeout.tv_sec = (long)(timeout_ms / 1000u);
  timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
  return select(0, write ? NULL : &set, write ? &set : NULL, NULL,
               &timeout);
#else
  struct pollfd descriptor;
  int timeout = timeout_ms > 2147483647u ? 2147483647 : (int)timeout_ms;
  descriptor.fd = socket;
  descriptor.events = (short)(write ? POLLOUT : POLLIN);
  descriptor.revents = 0;
  return poll(&descriptor, 1, timeout);
#endif
}

static int32_t v2_status_from_error(int error) {
  if (error == V2_EWOULDBLOCK || error == V2_EINPROGRESS)
    return CHTHOLLY_NEXT_HOST_V2_TIMEOUT;
  if (error == V2_ECONNRESET)
    return CHTHOLLY_NEXT_HOST_V2_RESET;
  return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
}

static int32_t v2_copy_endpoint(const struct sockaddr *address,
                                chtholly_next_host_v2_endpoint *out) {
  memset(out, 0, sizeof(*out));
  if (address->sa_family == AF_INET) {
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;
    out->family = 4;
    memcpy(out->address, &ipv4->sin_addr, 4);
    out->port = ntohs(ipv4->sin_port);
    return 0;
  }
  if (address->sa_family == AF_INET6) {
    const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)address;
    out->family = 6;
    memcpy(out->address, &ipv6->sin6_addr, 16);
    out->port = ntohs(ipv6->sin6_port);
    return 0;
  }
  return CHTHOLLY_NEXT_HOST_V2_UNSUPPORTED;
}

static int v2_make_address(const chtholly_next_host_v2_endpoint *endpoint,
                           struct sockaddr_storage *storage,
                           socklen_t *size) {
  memset(storage, 0, sizeof(*storage));
  if (endpoint->family == 4) {
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)storage;
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(endpoint->port);
    memcpy(&ipv4->sin_addr, endpoint->address, 4);
    *size = (socklen_t)sizeof(*ipv4);
    return AF_INET;
  }
  if (endpoint->family == 6) {
    struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)storage;
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(endpoint->port);
    memcpy(&ipv6->sin6_addr, endpoint->address, 16);
    *size = (socklen_t)sizeof(*ipv6);
    return AF_INET6;
  }
  return -1;
}

static int32_t v2_new_handle(v2_socket socket, void **out) {
  V2Handle *handle;
  if (out == NULL)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  *out = NULL;
  handle = (V2Handle *)calloc(1, sizeof(*handle));
  if (handle == NULL)
    return CHTHOLLY_NEXT_HOST_V2_OUT_OF_MEMORY;
  handle->fd = socket;
  handle->magic = V2_MAGIC;
  *out = handle;
  return 0;
}

int32_t chtholly_next_host_v2_resolve(const uint8_t *host, uint64_t host_size,
                                      uint16_t port,
                                      chtholly_next_host_v2_endpoint *out) {
  char *name;
  char service[16];
  struct addrinfo hints;
  struct addrinfo *result = NULL;
  struct addrinfo *cursor;
  int status;
  if (out == NULL || host == NULL || host_size == 0 || host_size > SIZE_MAX - 1)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  if (v2_startup() != 0)
    return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  name = (char *)malloc((size_t)host_size + 1u);
  if (name == NULL)
    return CHTHOLLY_NEXT_HOST_V2_OUT_OF_MEMORY;
  memcpy(name, host, (size_t)host_size);
  name[host_size] = '\0';
  snprintf(service, sizeof(service), "%u", (unsigned)port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  status = getaddrinfo(name, service, &hints, &result);
  free(name);
  if (status != 0)
    return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  status = CHTHOLLY_NEXT_HOST_V2_UNSUPPORTED;
  for (cursor = result; cursor != NULL; cursor = cursor->ai_next) {
    status = v2_copy_endpoint(cursor->ai_addr, out);
    if (status == 0)
      break;
  }
  freeaddrinfo(result);
  return status;
}

int32_t chtholly_next_host_v2_connect(
    const chtholly_next_host_v2_endpoint *endpoint, uint64_t timeout_ms,
    void **out_stream) {
  struct sockaddr_storage address;
  socklen_t address_size;
  v2_socket fd;
  int family;
#if !defined(_WIN32)
  int flags = 0;
#endif
  int status;
  if (endpoint == NULL || out_stream == NULL)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  family = v2_make_address(endpoint, &address, &address_size);
  if (family < 0)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  if (v2_startup() != 0)
    return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  fd = socket(family, SOCK_STREAM, 0);
  if (fd == V2_INVALID_SOCKET)
    return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
#if defined(_WIN32)
  { u_long nonblocking = 1; ioctlsocket(fd, FIONBIO, &nonblocking); }
#else
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    v2_close_socket(fd); return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  }
#endif
  status = connect(fd, (const struct sockaddr *)&address, address_size);
  if (status != 0) {
    if (v2_last_error != V2_EINPROGRESS && v2_last_error != V2_EWOULDBLOCK) {
      status = v2_status_from_error(v2_last_error);
      v2_close_socket(fd); return status;
    }
    status = v2_wait(fd, 1, timeout_ms);
    if (status <= 0) {
      v2_close_socket(fd);
      return status == 0 ? CHTHOLLY_NEXT_HOST_V2_TIMEOUT
                         : CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
    }
#if defined(_WIN32)
    {
      int socket_error = 0;
      int socket_error_size = (int)sizeof(socket_error);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&socket_error,
                 &socket_error_size);
      if (socket_error != 0) {
        status = v2_status_from_error(socket_error);
        v2_close_socket(fd);
        return status;
      }
    }
#else
    {
      int socket_error = 0;
      socklen_t socket_error_size = (socklen_t)sizeof(socket_error);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                     &socket_error_size) != 0 || socket_error != 0) {
        status = v2_status_from_error(socket_error);
        v2_close_socket(fd);
        return status;
      }
    }
#endif
  }
  return v2_new_handle(fd, out_stream);
}

int32_t chtholly_next_host_v2_bind(
    const chtholly_next_host_v2_endpoint *endpoint, void **out_listener) {
  struct sockaddr_storage address;
  socklen_t address_size;
  v2_socket fd;
  int family;
  int reuse = 1;
  if (endpoint == NULL || out_listener == NULL)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  family = v2_make_address(endpoint, &address, &address_size);
  if (family < 0 || v2_startup() != 0)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  fd = socket(family, SOCK_STREAM, 0);
  if (fd == V2_INVALID_SOCKET)
    return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
             (socklen_t)sizeof(reuse));
  if (bind(fd, (const struct sockaddr *)&address, address_size) != 0 ||
      listen(fd, 64) != 0) {
    v2_close_socket(fd); return CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  }
  return v2_new_handle(fd, out_listener);
}

int32_t chtholly_next_host_v2_accept(void *listener, uint64_t timeout_ms,
                                     void **out_stream) {
  V2Handle *handle = v2_handle(listener);
  v2_socket accepted;
  if (handle == NULL || out_stream == NULL)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  {
    const int ready = v2_wait(handle->fd, 0, timeout_ms);
    if (ready <= 0)
      return ready == 0 ? CHTHOLLY_NEXT_HOST_V2_TIMEOUT
                        : CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  }
  accepted = accept(handle->fd, NULL, NULL);
  if (accepted == V2_INVALID_SOCKET)
    return v2_status_from_error(v2_last_error);
  return v2_new_handle(accepted, out_stream);
}

int64_t chtholly_next_host_v2_read(void *stream, uint8_t *buffer,
                                   uint64_t capacity, uint64_t timeout_ms) {
  V2Handle *handle = v2_handle(stream);
  int result;
  if (handle == NULL || buffer == NULL || capacity == 0)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  result = v2_wait(handle->fd, 0, timeout_ms);
  if (result <= 0)
    return result == 0 ? CHTHOLLY_NEXT_HOST_V2_TIMEOUT
                       : CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  result = recv(handle->fd, (char *)buffer,
                (int)(capacity > INT_MAX ? INT_MAX : capacity), 0);
  if (result == 0)
    return CHTHOLLY_NEXT_HOST_V2_CLOSED;
  if (result < 0)
    return v2_status_from_error(v2_last_error);
  return result;
}

int64_t chtholly_next_host_v2_write(const void *opaque, const uint8_t *buffer,
                                    uint64_t size, uint64_t timeout_ms) {
  V2Handle *handle = v2_handle((void *)opaque);
  int result;
  if (handle == NULL || buffer == NULL || size == 0)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  result = v2_wait(handle->fd, 1, timeout_ms);
  if (result <= 0)
    return result == 0 ? CHTHOLLY_NEXT_HOST_V2_TIMEOUT
                       : CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
  result = send(handle->fd, (const char *)buffer,
                (int)(size > INT_MAX ? INT_MAX : size), 0);
  if (result < 0)
    return v2_status_from_error(v2_last_error);
  return result;
}

int32_t chtholly_next_host_v2_shutdown(void *stream, int32_t mode) {
  V2Handle *handle = v2_handle(stream);
  int how = mode == 0 ? V2_SHUT_RD : mode == 1 ? V2_SHUT_WR : V2_SHUT_RDWR;
  if (handle == NULL || (mode < 0 || mode > 2))
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  return shutdown(handle->fd, how) == 0 ? 0
                                             : CHTHOLLY_NEXT_HOST_V2_IO_FAILURE;
}

int32_t chtholly_next_host_v2_close(void *opaque) {
  V2Handle *handle = v2_handle(opaque);
  if (handle == NULL)
    return CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT;
  v2_close_socket(handle->fd);
  handle->magic = 0;
  free(handle);
  return 0;
}
