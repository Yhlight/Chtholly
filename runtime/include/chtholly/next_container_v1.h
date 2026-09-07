#ifndef CHTHOLLY_NEXT_CONTAINER_V1_H
#define CHTHOLLY_NEXT_CONTAINER_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTHOLLY_NEXT_CONTAINER_V1_MAGIC UINT32_C(0x43485431)
#define CHTHOLLY_NEXT_CONTAINER_V1_ABI UINT16_C(1)

typedef uint64_t (*chtholly_next_container_v1_hash_fn)(
    const void *object, uint64_t seed, void *context);
typedef int32_t (*chtholly_next_container_v1_equal_fn)(
    const void *left, const void *right, void *context);
typedef void (*chtholly_next_container_v1_move_fn)(
    void *destination, void *source, void *context);
typedef void (*chtholly_next_container_v1_drop_fn)(void *object,
                                                   void *context);
typedef void (*chtholly_next_container_v1_borrow_invalidate_fn)(
    void *context, uint64_t old_generation, uint64_t new_generation);
typedef int32_t (*chtholly_next_container_v1_rehash_begin_fn)(
    void *context, uint64_t old_capacity, uint64_t new_capacity);
typedef void (*chtholly_next_container_v1_rehash_commit_fn)(
    void *context, uint64_t old_generation, uint64_t new_generation);
typedef void (*chtholly_next_container_v1_rehash_abort_fn)(void *context);

typedef struct chtholly_next_container_v1_vtable {
  uint32_t magic;
  uint16_t abi_version;
  uint16_t flags;
  uint32_t semantic_epoch;
  uint32_t target_pointer_bits;
  uint64_t key_size;
  uint64_t key_alignment;
  uint64_t value_size;
  uint64_t value_alignment;
  uint8_t key_type_fingerprint[32];
  uint8_t value_type_fingerprint[32];
  uint8_t layout_fingerprint[32];
  void *context;
  chtholly_next_container_v1_hash_fn hash;
  chtholly_next_container_v1_equal_fn equal;
  chtholly_next_container_v1_move_fn move_key;
  chtholly_next_container_v1_move_fn move_value;
  chtholly_next_container_v1_drop_fn drop_key;
  chtholly_next_container_v1_drop_fn drop_value;
  chtholly_next_container_v1_borrow_invalidate_fn borrow_invalidate;
  chtholly_next_container_v1_rehash_begin_fn rehash_begin;
  chtholly_next_container_v1_rehash_commit_fn rehash_commit;
  chtholly_next_container_v1_rehash_abort_fn rehash_abort;
} chtholly_next_container_v1_vtable;

typedef struct chtholly_next_container_v1_table {
  void *metadata;
  void *keys;
  void *values;
  uint64_t size;
  uint64_t capacity;
  uint64_t deleted;
  uint64_t generation;
  uint64_t seed;
  const chtholly_next_container_v1_vtable *vtable;
  void *context;
} chtholly_next_container_v1_table;

typedef enum chtholly_next_container_v1_status {
  CHTHOLLY_NEXT_CONTAINER_V1_OK = 0,
  CHTHOLLY_NEXT_CONTAINER_V1_NOT_FOUND = 1,
  CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY = 2,
  CHTHOLLY_NEXT_CONTAINER_V1_CAPACITY_OVERFLOW = 3,
  CHTHOLLY_NEXT_CONTAINER_V1_INVALID_DESCRIPTOR = 4,
  CHTHOLLY_NEXT_CONTAINER_V1_INVALID_ARGUMENT = 5,
  CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW = 6
} chtholly_next_container_v1_status;

int32_t chtholly_next_container_v1_validate_vtable(
    const chtholly_next_container_v1_vtable *vtable,
    uint32_t semantic_epoch, uint32_t target_pointer_bits);
int32_t chtholly_next_container_v1_init(
    chtholly_next_container_v1_table *table,
    const chtholly_next_container_v1_vtable *vtable, void *context,
    uint64_t seed);
chtholly_next_container_v1_table *chtholly_next_container_v1_create(
    const chtholly_next_container_v1_vtable *vtable, void *context,
    uint64_t seed, int32_t *status);
void chtholly_next_container_v1_destroy(
    chtholly_next_container_v1_table *table);
uint64_t chtholly_next_container_v1_size(
    const chtholly_next_container_v1_table *table);
uint64_t chtholly_next_container_v1_capacity(
    const chtholly_next_container_v1_table *table);
uint64_t chtholly_next_container_v1_generation(
    const chtholly_next_container_v1_table *table);
int32_t chtholly_next_container_v1_reserve(
    chtholly_next_container_v1_table *table, uint64_t requested);
int32_t chtholly_next_container_v1_find(
    const chtholly_next_container_v1_table *table, const void *key,
    const void **value);
int32_t chtholly_next_container_v1_insert(
    chtholly_next_container_v1_table *table, const void *key,
    const void *value, void *replaced_value, uint8_t *replaced);
int32_t chtholly_next_container_v1_erase(
    chtholly_next_container_v1_table *table, const void *key,
    void *removed_value, uint8_t *removed);
int32_t chtholly_next_container_v1_clear(
    chtholly_next_container_v1_table *table);
int32_t chtholly_next_container_v1_borrow_is_valid(
    const chtholly_next_container_v1_table *table, uint64_t generation);

/*
 * Deterministic allocator controls are deliberately test-only.  They are
 * compiled into the runtime test target, but are not part of the stable
 * component/container ABI.
 */
#if defined(CHTHOLLY_RUNTIME_TESTING)
void chtholly_next_container_v1_testing_allocator_reset(void);
void chtholly_next_container_v1_testing_allocator_fail_after(
    uint64_t allocation_attempt);
uint64_t chtholly_next_container_v1_testing_allocator_attempts(void);
uint64_t chtholly_next_container_v1_testing_allocator_failures(void);
uint64_t chtholly_next_container_v1_testing_allocator_frees(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
