#ifndef CHTHOLLY_COMPONENT_ABI_V2_H
#define CHTHOLLY_COMPONENT_ABI_V2_H
#include <stdint.h>
#include "chtholly/next_resource_lease_v2.h"
#ifdef __cplusplus
extern "C" {
#endif
#define CHTHOLLY_COMPONENT_ABI_EPOCH_V2 2u
#define CHTHOLLY_COMPONENT_QUERY_SYMBOL_V2 "chtholly_component_query_v2"
#define CHTHOLLY_COMPONENT_INVOKE_SYMBOL_V2 "chtholly_component_invoke_v2"
#define CHTHOLLY_COMPONENT_CLOSE_SYMBOL_V2 "chtholly_component_close_v2"
typedef struct chtholly_component_invocation_v2 {
  /* struct_size permits only a same-prefix extension; reserved fields must be
   * zero and providers must reject sizes smaller than the consumed prefix. */
  uint32_t struct_size;
  uint32_t abi_epoch;
  uint8_t operation_kind;
  uint8_t reserved[3];
  uint8_t descriptor_digest[32];
  uint8_t payload_type_digest[32];
  uint8_t layout_digest[32];
  uint8_t lifecycle_digest[32];
  uint8_t contract_digest[32];
  uint8_t plan_fingerprint[32];
  uint64_t payload_size;
  uint32_t source_lane;
  uint32_t destination_lane;
  uint32_t token_lane;
  uint32_t reserved_tail;
  chtholly_next_resource_operation_v2 *operation;
  chtholly_next_payload_transport_v2 *transport;
} chtholly_component_invocation_v2;
typedef int32_t (*chtholly_component_query_fn_v2)(const uint8_t **bytes, uint64_t *size);
typedef int32_t (*chtholly_component_invoke_fn_v2)(const chtholly_component_invocation_v2 *invocation);
typedef int32_t (*chtholly_component_close_fn_v2)(void);
#ifdef __cplusplus
}
#endif
#endif
