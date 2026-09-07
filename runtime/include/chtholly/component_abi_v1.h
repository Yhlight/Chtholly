#ifndef CHTHOLLY_COMPONENT_ABI_V1_H
#define CHTHOLLY_COMPONENT_ABI_V1_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define CHTHOLLY_COMPONENT_EXPORT __declspec(dllexport)
#else
#define CHTHOLLY_COMPONENT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_COMPONENT_ABI_EPOCH_V1 1u
#define CHTHOLLY_COMPONENT_QUERY_SYMBOL_V1 "chtholly_component_query_v1"
#define CHTHOLLY_COMPONENT_DIGEST_SIZE_V1 32u

typedef enum chtholly_component_value_kind_v1 {
  CHTHOLLY_COMPONENT_VALUE_VOID_V1 = 0,
  CHTHOLLY_COMPONENT_VALUE_BOOL_V1 = 1,
  CHTHOLLY_COMPONENT_VALUE_I8_V1 = 2,
  CHTHOLLY_COMPONENT_VALUE_U8_V1 = 3,
  CHTHOLLY_COMPONENT_VALUE_I16_V1 = 4,
  CHTHOLLY_COMPONENT_VALUE_U16_V1 = 5,
  CHTHOLLY_COMPONENT_VALUE_I32_V1 = 6,
  CHTHOLLY_COMPONENT_VALUE_U32_V1 = 7,
  CHTHOLLY_COMPONENT_VALUE_I64_V1 = 8,
  CHTHOLLY_COMPONENT_VALUE_U64_V1 = 9,
  CHTHOLLY_COMPONENT_VALUE_F32_V1 = 10,
  CHTHOLLY_COMPONENT_VALUE_F64_V1 = 11,
  CHTHOLLY_COMPONENT_VALUE_BYTES_V1 = 12
} chtholly_component_value_kind_v1;

typedef struct chtholly_component_bytes_v1 {
  const uint8_t *data;
  uint64_t size;
} chtholly_component_bytes_v1;

typedef union chtholly_component_value_payload_v1 {
  uint64_t bits;
  chtholly_component_bytes_v1 bytes;
} chtholly_component_value_payload_v1;

typedef struct chtholly_component_value_v1 {
  uint32_t struct_size;
  uint32_t kind;
  uint32_t flags;
  uint32_t reserved;
  chtholly_component_value_payload_v1 payload;
} chtholly_component_value_v1;

typedef enum chtholly_component_invoke_status_v1 {
  CHTHOLLY_COMPONENT_INVOKE_OK_V1 = 0,
  CHTHOLLY_COMPONENT_INVOKE_INVALID_ARGUMENT_V1 = 1,
  CHTHOLLY_COMPONENT_INVOKE_INVALID_RESULT_V1 = 2
} chtholly_component_invoke_status_v1;

typedef uint32_t (*chtholly_component_invoke_fn_v1)(
    const chtholly_component_value_v1 *arguments, uint32_t argument_count,
    chtholly_component_value_v1 *result);

typedef struct chtholly_component_export_descriptor_v1 {
  uint32_t struct_size;
  uint32_t flags;
  uint8_t export_id[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t signature_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  const char *canonical_name;
  uint64_t canonical_name_size;
  chtholly_component_invoke_fn_v1 invoke;
  uint64_t reserved[2];
} chtholly_component_export_descriptor_v1;

typedef struct chtholly_component_descriptor_v1 {
  uint32_t struct_size;
  uint32_t abi_epoch;
  uint32_t flags;
  uint32_t reserved;
  uint8_t identity_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t contract_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t target_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t runtime_abi_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  const char *identity;
  uint64_t identity_size;
  const chtholly_component_export_descriptor_v1 *exports;
  uint64_t export_count;
  uint64_t reserved_words[4];
} chtholly_component_descriptor_v1;

typedef const chtholly_component_descriptor_v1 *(
    *chtholly_component_query_fn_v1)(void);

#ifdef __cplusplus
}
#endif

#endif
