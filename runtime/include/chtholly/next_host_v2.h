#ifndef CHTHOLLY_NEXT_HOST_V2_H
#define CHTHOLLY_NEXT_HOST_V2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_NEXT_HOST_ABI_V2 2u
#define CHTHOLLY_NEXT_HOST_V2_OK 0
#define CHTHOLLY_NEXT_HOST_V2_INVALID_ARGUMENT (-2801)
#define CHTHOLLY_NEXT_HOST_V2_IO_FAILURE (-2802)
#define CHTHOLLY_NEXT_HOST_V2_OUT_OF_MEMORY (-2803)
#define CHTHOLLY_NEXT_HOST_V2_TIMEOUT (-2804)
#define CHTHOLLY_NEXT_HOST_V2_CLOSED (-2805)
#define CHTHOLLY_NEXT_HOST_V2_RESET (-2806)
#define CHTHOLLY_NEXT_HOST_V2_UNSUPPORTED (-2807)

typedef struct chtholly_next_host_v2_endpoint {
  uint8_t family;
  uint8_t address[16];
  uint16_t port;
  uint16_t reserved;
} chtholly_next_host_v2_endpoint;

int32_t chtholly_next_host_v2_resolve(const uint8_t *host,
                                      uint64_t host_size, uint16_t port,
                                      chtholly_next_host_v2_endpoint *out);
int32_t chtholly_next_host_v2_connect(
    const chtholly_next_host_v2_endpoint *endpoint, uint64_t timeout_ms,
    void **out_stream);
int32_t chtholly_next_host_v2_bind(
    const chtholly_next_host_v2_endpoint *endpoint, void **out_listener);
int32_t chtholly_next_host_v2_accept(void *listener, uint64_t timeout_ms,
                                     void **out_stream);
int64_t chtholly_next_host_v2_read(void *stream, uint8_t *buffer,
                                   uint64_t capacity, uint64_t timeout_ms);
int64_t chtholly_next_host_v2_write(const void *stream, const uint8_t *buffer,
                                    uint64_t size, uint64_t timeout_ms);
int32_t chtholly_next_host_v2_shutdown(void *stream, int32_t mode);
int32_t chtholly_next_host_v2_close(void *handle);

#ifdef __cplusplus
}
#endif

#endif
