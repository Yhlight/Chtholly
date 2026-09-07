#ifndef CHTHOLLY_COMPONENT_LOADER_V2_H
#define CHTHOLLY_COMPONENT_LOADER_V2_H
#include <stdint.h>
#include "chtholly/next_resource_lease_v2.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct chtholly_component_module_v2 chtholly_component_module_v2;
/* close_v2 is retryable: CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY means an
 * armed operation still owns the module lease. Callers must retry close_v2
 * until OK before unload_v2/release_v2. unload_v2 is close_v2 followed by
 * release_v2 and consumes the module handle only on success. */
enum {
  CHTHOLLY_COMPONENT_LOADER_V2_OK = 0,
  CHTHOLLY_COMPONENT_LOADER_V2_INVALID_ARGUMENT = 1,
  CHTHOLLY_COMPONENT_LOADER_V2_PLATFORM_ERROR = 2,
  CHTHOLLY_COMPONENT_LOADER_V2_QUERY_MISSING = 3,
  CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_INVALID = 4,
  CHTHOLLY_COMPONENT_LOADER_V2_ABI_MISMATCH = 5,
  CHTHOLLY_COMPONENT_LOADER_V2_CLOSING = 6,
  CHTHOLLY_COMPONENT_LOADER_V2_OPERATION_FAILED = 7,
  CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY = 8,
  CHTHOLLY_COMPONENT_LOADER_V2_DESCRIPTOR_MISMATCH = 9
};
uint32_t chtholly_component_load_v2(const char *, chtholly_component_module_v2 **, char *, uint64_t, uint64_t *);
uint32_t chtholly_component_load_v2_from_artifact(const char *, const char *, chtholly_component_module_v2 **, char *, uint64_t, uint64_t *);
uint32_t chtholly_component_invoke_v2(chtholly_component_module_v2 *, uint8_t, struct chtholly_next_resource_operation_v2 **, char *, uint64_t, uint64_t *);
uint32_t chtholly_component_invoke_payload_v2(
    chtholly_component_module_v2 *, uint8_t,
    chtholly_next_payload_transport_v2 *,
    struct chtholly_next_resource_operation_v2 **, char *, uint64_t, uint64_t *);
uint32_t chtholly_component_close_v2(chtholly_component_module_v2 *, char *, uint64_t, uint64_t *);
int32_t chtholly_component_retain_owner_v2(void *module);
void chtholly_component_release_owner_v2(void *module);
uint32_t chtholly_component_release_v2(chtholly_component_module_v2 *);
uint32_t chtholly_component_unload_v2(chtholly_component_module_v2 *, char *, uint64_t, uint64_t *);
#ifdef __cplusplus
}
#endif
#endif
