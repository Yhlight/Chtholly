#ifndef CHTHOLLY_COMPONENT_LOADER_V1_H
#define CHTHOLLY_COMPONENT_LOADER_V1_H

#include "chtholly/component_abi_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chtholly_component_module_v1 chtholly_component_module_v1;

typedef enum chtholly_component_loader_status_v1 {
  CHTHOLLY_COMPONENT_LOADER_OK_V1 = 0,
  CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1 = 1,
  CHTHOLLY_COMPONENT_LOADER_PLATFORM_ERROR_V1 = 2,
  CHTHOLLY_COMPONENT_LOADER_QUERY_MISSING_V1 = 3,
  CHTHOLLY_COMPONENT_LOADER_DESCRIPTOR_INVALID_V1 = 4,
  CHTHOLLY_COMPONENT_LOADER_ABI_MISMATCH_V1 = 5,
  CHTHOLLY_COMPONENT_LOADER_IDENTITY_MISMATCH_V1 = 6,
  CHTHOLLY_COMPONENT_LOADER_CONTRACT_MISMATCH_V1 = 7,
  CHTHOLLY_COMPONENT_LOADER_TARGET_MISMATCH_V1 = 8,
  CHTHOLLY_COMPONENT_LOADER_RUNTIME_MISMATCH_V1 = 9,
  CHTHOLLY_COMPONENT_LOADER_EXPORT_NOT_FOUND_V1 = 10,
  CHTHOLLY_COMPONENT_LOADER_CLOSING_V1 = 11,
  CHTHOLLY_COMPONENT_LOADER_INVOKE_FAILED_V1 = 12,
  CHTHOLLY_COMPONENT_LOADER_BUFFER_TOO_SMALL_V1 = 13
} chtholly_component_loader_status_v1;

typedef struct chtholly_component_requirement_v1 {
  uint32_t struct_size;
  uint32_t abi_epoch;
  uint8_t identity_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t contract_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t target_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t runtime_abi_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint64_t reserved[4];
} chtholly_component_requirement_v1;

typedef struct chtholly_component_export_info_v1 {
  uint32_t struct_size;
  uint32_t flags;
  uint8_t export_id[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint8_t signature_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1];
  uint64_t canonical_name_size;
} chtholly_component_export_info_v1;

uint32_t chtholly_component_requirement_init_v1(
    const char *identity_utf8, uint64_t identity_size,
    const uint8_t contract_digest[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1],
    const char *normalized_target_utf8, uint64_t target_size,
    const char *runtime_abi_utf8, uint64_t runtime_abi_size,
    chtholly_component_requirement_v1 *out_requirement, char *diagnostic,
    uint64_t diagnostic_capacity, uint64_t *diagnostic_size);

uint32_t
chtholly_component_load_v1(const char *absolute_path_utf8,
                           const chtholly_component_requirement_v1 *requirement,
                           chtholly_component_module_v1 **out_module,
                           char *diagnostic, uint64_t diagnostic_capacity,
                           uint64_t *diagnostic_size);

uint32_t
chtholly_component_export_count_v1(const chtholly_component_module_v1 *module,
                                   uint64_t *out_count);

uint32_t chtholly_component_export_info_v1_get(
    const chtholly_component_module_v1 *module, uint64_t index,
    chtholly_component_export_info_v1 *out_info, char *name,
    uint64_t name_capacity, uint64_t *name_size);

uint32_t chtholly_component_invoke_v1(
    chtholly_component_module_v1 *module,
    const uint8_t export_id[CHTHOLLY_COMPONENT_DIGEST_SIZE_V1],
    const chtholly_component_value_v1 *arguments, uint32_t argument_count,
    chtholly_component_value_v1 *result, char *diagnostic,
    uint64_t diagnostic_capacity, uint64_t *diagnostic_size);

uint32_t chtholly_component_close_v1(chtholly_component_module_v1 *module,
                                     char *diagnostic,
                                     uint64_t diagnostic_capacity,
                                     uint64_t *diagnostic_size);

uint32_t chtholly_component_release_v1(chtholly_component_module_v1 *module);

uint32_t chtholly_component_unload_v1(chtholly_component_module_v1 *module,
                                      char *diagnostic,
                                      uint64_t diagnostic_capacity,
                                      uint64_t *diagnostic_size);

#ifdef __cplusplus
}
#endif

#endif
