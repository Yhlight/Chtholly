#include "chtholly/next_container_v1.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { EMPTY = 0, OCCUPIED = 1, DELETED = 2 };

#if defined(CHTHOLLY_RUNTIME_TESTING)
typedef struct chtholly_next_container_v1_testing_allocator_state {
  uint64_t fail_after;
  uint64_t attempts;
  uint64_t failures;
  uint64_t frees;
} chtholly_next_container_v1_testing_allocator_state;

static chtholly_next_container_v1_testing_allocator_state
    testing_allocator_state;

static int testing_allocator_should_fail(void) {
  ++testing_allocator_state.attempts;
  if (testing_allocator_state.fail_after != 0 &&
      testing_allocator_state.attempts == testing_allocator_state.fail_after) {
    ++testing_allocator_state.failures;
    return 1;
  }
  return 0;
}

static void *testing_malloc(size_t size) {
  if (testing_allocator_should_fail())
    return NULL;
  return malloc(size);
}

static void *testing_calloc(size_t count, size_t size) {
  if (testing_allocator_should_fail())
    return NULL;
  return calloc(count, size);
}

static void testing_free(void *pointer) {
  if (pointer)
    ++testing_allocator_state.frees;
  free(pointer);
}

void chtholly_next_container_v1_testing_allocator_reset(void) {
  memset(&testing_allocator_state, 0, sizeof(testing_allocator_state));
}

void chtholly_next_container_v1_testing_allocator_fail_after(
    uint64_t allocation_attempt) {
  testing_allocator_state.fail_after = allocation_attempt;
}

uint64_t chtholly_next_container_v1_testing_allocator_attempts(void) {
  return testing_allocator_state.attempts;
}

uint64_t chtholly_next_container_v1_testing_allocator_failures(void) {
  return testing_allocator_state.failures;
}

uint64_t chtholly_next_container_v1_testing_allocator_frees(void) {
  return testing_allocator_state.frees;
}
#define CHTHOLLY_CONTAINER_MALLOC testing_malloc
#define CHTHOLLY_CONTAINER_CALLOC testing_calloc
#define CHTHOLLY_CONTAINER_FREE testing_free
#else
#define CHTHOLLY_CONTAINER_MALLOC malloc
#define CHTHOLLY_CONTAINER_CALLOC calloc
#define CHTHOLLY_CONTAINER_FREE free
#endif

static int valid(const chtholly_next_container_v1_vtable *v, uint32_t epoch,
                 uint32_t pointer_bits) {
  int key_fingerprint = 0, value_fingerprint = 0, layout_fingerprint = 0;
  if (v) {
    for (size_t i = 0; i < 32; ++i) {
      key_fingerprint |= v->key_type_fingerprint[i] != 0;
      value_fingerprint |= v->value_type_fingerprint[i] != 0;
      layout_fingerprint |= v->layout_fingerprint[i] != 0;
    }
  }
  if (!v || v->magic != CHTHOLLY_NEXT_CONTAINER_V1_MAGIC ||
      v->abi_version != CHTHOLLY_NEXT_CONTAINER_V1_ABI ||
      v->semantic_epoch != epoch || v->target_pointer_bits != pointer_bits ||
      v->key_size == 0 || v->value_size == 0 || v->key_alignment == 0 ||
      v->value_alignment == 0 || !key_fingerprint || !value_fingerprint ||
      !layout_fingerprint || !v->hash || !v->equal || !v->move_key ||
      !v->move_value || !v->drop_key || !v->drop_value)
    return 0;
  if (v->key_size > SIZE_MAX || v->value_size > SIZE_MAX ||
      /* MSVC's C11 library does not expose max_align_t; 16 is the ABI
         maximum for the supported Chtholly object representations. */
      v->key_alignment > 16 || v->value_alignment > 16 ||
      (v->key_alignment & (v->key_alignment - 1)) != 0 ||
      (v->value_alignment & (v->value_alignment - 1)) != 0)
    return 0;
  return 1;
}

static uint8_t *meta(const chtholly_next_container_v1_table *t) {
  return (uint8_t *)t->metadata;
}
static uint8_t *key_at(const chtholly_next_container_v1_table *t,
                       uint64_t slot) {
  return (uint8_t *)t->keys + slot * t->vtable->key_size;
}
static uint8_t *value_at(const chtholly_next_container_v1_table *t,
                         uint64_t slot) {
  return (uint8_t *)t->values + slot * t->vtable->value_size;
}

static uint64_t probe(uint64_t hash, uint64_t step, uint64_t capacity) {
  const uint64_t offset = step + step * step;
  return (hash + offset / 2) & (capacity - 1);
}

static int bump_generation(chtholly_next_container_v1_table *t) {
  if (t->generation == UINT64_MAX)
    return CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW;
  const uint64_t old = t->generation++;
  if (t->vtable->borrow_invalidate)
    t->vtable->borrow_invalidate(t->context, old, t->generation);
  return CHTHOLLY_NEXT_CONTAINER_V1_OK;
}

int32_t chtholly_next_container_v1_validate_vtable(
    const chtholly_next_container_v1_vtable *vtable, uint32_t epoch,
    uint32_t pointer_bits) {
  return valid(vtable, epoch, pointer_bits)
             ? CHTHOLLY_NEXT_CONTAINER_V1_OK
             : CHTHOLLY_NEXT_CONTAINER_V1_INVALID_DESCRIPTOR;
}

int32_t chtholly_next_container_v1_init(
    chtholly_next_container_v1_table *t,
    const chtholly_next_container_v1_vtable *vtable, void *context,
    uint64_t seed) {
  if (!t || !valid(vtable, vtable ? vtable->semantic_epoch : 0,
                   vtable ? vtable->target_pointer_bits : 0))
    return CHTHOLLY_NEXT_CONTAINER_V1_INVALID_DESCRIPTOR;
  memset(t, 0, sizeof(*t));
  t->vtable = vtable;
  t->context = context ? context : vtable->context;
  t->seed = seed;
  return CHTHOLLY_NEXT_CONTAINER_V1_OK;
}

chtholly_next_container_v1_table *chtholly_next_container_v1_create(
    const chtholly_next_container_v1_vtable *vtable, void *context,
    uint64_t seed, int32_t *status) {
  if (!status)
    return NULL;
  *status = CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY;
  chtholly_next_container_v1_table *table =
      (chtholly_next_container_v1_table *)CHTHOLLY_CONTAINER_CALLOC(
          1, sizeof(*table));
  if (!table)
    return NULL;
  *status = chtholly_next_container_v1_init(table, vtable, context, seed);
  if (*status != CHTHOLLY_NEXT_CONTAINER_V1_OK) {
    CHTHOLLY_CONTAINER_FREE(table);
    return NULL;
  }
  return table;
}

static void drop_slots(chtholly_next_container_v1_table *t) {
  if (!t || !t->metadata || !t->vtable)
    return;
  for (uint64_t i = 0; i < t->capacity; ++i) {
    if (meta(t)[i] != OCCUPIED)
      continue;
    t->vtable->drop_key(key_at(t, i), t->context);
    t->vtable->drop_value(value_at(t, i), t->context);
  }
}

void chtholly_next_container_v1_destroy(chtholly_next_container_v1_table *t) {
  if (!t)
    return;
  drop_slots(t);
  CHTHOLLY_CONTAINER_FREE(t->metadata);
  CHTHOLLY_CONTAINER_FREE(t->keys);
  CHTHOLLY_CONTAINER_FREE(t->values);
  memset(t, 0, sizeof(*t));
}

uint64_t chtholly_next_container_v1_size(
    const chtholly_next_container_v1_table *t) {
  return t ? t->size : 0;
}
uint64_t chtholly_next_container_v1_capacity(
    const chtholly_next_container_v1_table *t) {
  return t ? t->capacity : 0;
}
uint64_t chtholly_next_container_v1_generation(
    const chtholly_next_container_v1_table *t) {
  return t ? t->generation : 0;
}

static int allocate_arrays(uint64_t capacity, const void **metadata,
                           const void **keys, const void **values,
                           const chtholly_next_container_v1_vtable *v) {
  if (capacity > SIZE_MAX / (v->key_size ? v->key_size : 1) ||
      capacity > SIZE_MAX / (v->value_size ? v->value_size : 1))
    return 0;
  void *m = CHTHOLLY_CONTAINER_CALLOC((size_t)capacity, 1);
  void *k = CHTHOLLY_CONTAINER_CALLOC((size_t)capacity, (size_t)v->key_size);
  void *value =
      CHTHOLLY_CONTAINER_CALLOC((size_t)capacity, (size_t)v->value_size);
  if (!m || !k || !value) {
    CHTHOLLY_CONTAINER_FREE(m);
    CHTHOLLY_CONTAINER_FREE(k);
    CHTHOLLY_CONTAINER_FREE(value);
    return 0;
  }
  *metadata = m;
  *keys = k;
  *values = value;
  return 1;
}

static void free_arrays(const void *metadata, const void *keys,
                        const void *values) {
  CHTHOLLY_CONTAINER_FREE((void *)metadata);
  CHTHOLLY_CONTAINER_FREE((void *)keys);
  CHTHOLLY_CONTAINER_FREE((void *)values);
}

int32_t chtholly_next_container_v1_reserve(
    chtholly_next_container_v1_table *t, uint64_t requested) {
  if (!t || !t->vtable)
    return CHTHOLLY_NEXT_CONTAINER_V1_INVALID_ARGUMENT;
  if (requested <= t->capacity)
    return CHTHOLLY_NEXT_CONTAINER_V1_OK;
  if (t->generation == UINT64_MAX)
    return CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW;
  uint64_t capacity = t->capacity ? t->capacity : 8;
  while (capacity < requested) {
    if (capacity > UINT64_MAX / 2)
      return CHTHOLLY_NEXT_CONTAINER_V1_CAPACITY_OVERFLOW;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX)
    return CHTHOLLY_NEXT_CONTAINER_V1_CAPACITY_OVERFLOW;
  const void *new_meta = NULL, *new_keys = NULL, *new_values = NULL;
  if (!allocate_arrays(capacity, &new_meta, &new_keys, &new_values, t->vtable))
    return CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY;
  if (t->vtable->rehash_begin) {
    const int32_t begin = t->vtable->rehash_begin(
        t->context, t->capacity, capacity);
    if (begin != CHTHOLLY_NEXT_CONTAINER_V1_OK) {
      free_arrays(new_meta, new_keys, new_values);
      if (t->vtable->rehash_abort)
        t->vtable->rehash_abort(t->context);
      return begin;
    }
  }
  uint64_t *relocation = NULL;
  if (t->size != 0) {
    if (t->size > SIZE_MAX / sizeof(*relocation)) {
      free_arrays(new_meta, new_keys, new_values);
      if (t->vtable->rehash_abort)
        t->vtable->rehash_abort(t->context);
      return CHTHOLLY_NEXT_CONTAINER_V1_CAPACITY_OVERFLOW;
    }
    relocation = (uint64_t *)CHTHOLLY_CONTAINER_MALLOC(
        (size_t)t->size * sizeof(*relocation));
    if (!relocation) {
      free_arrays(new_meta, new_keys, new_values);
      if (t->vtable->rehash_abort)
        t->vtable->rehash_abort(t->context);
      return CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY;
    }
  }
  chtholly_next_container_v1_table candidate = *t;
  candidate.metadata = (void *)new_meta;
  candidate.keys = (void *)new_keys;
  candidate.values = (void *)new_values;
  candidate.capacity = capacity;
  candidate.size = 0;
  candidate.deleted = 0;
  uint64_t relocation_index = 0;
  for (uint64_t i = 0; i < t->capacity; ++i) {
    if (meta(t)[i] != OCCUPIED)
      continue;
    const uint64_t hash = t->vtable->hash(key_at(t, i), t->seed, t->context);
    uint64_t slot = UINT64_MAX;
    for (uint64_t step = 0; step < capacity; ++step) {
      const uint64_t candidate_slot = probe(hash, step, capacity);
      if (((uint8_t *)candidate.metadata)[candidate_slot] == EMPTY) {
        slot = candidate_slot;
        break;
      }
    }
    if (slot == UINT64_MAX) {
      CHTHOLLY_CONTAINER_FREE(relocation);
      free_arrays(new_meta, new_keys, new_values);
      if (t->vtable->rehash_abort)
        t->vtable->rehash_abort(t->context);
      return CHTHOLLY_NEXT_CONTAINER_V1_CAPACITY_OVERFLOW;
    }
    ((uint8_t *)candidate.metadata)[slot] = OCCUPIED;
    relocation[relocation_index++] = slot;
    candidate.size++;
  }
  /* Every fallible step is complete. The callbacks are no-throw, so moving
     objects below cannot invalidate the old table before commit. */
  relocation_index = 0;
  for (uint64_t i = 0; i < t->capacity; ++i) {
    if (meta(t)[i] != OCCUPIED)
      continue;
    const uint64_t slot = relocation[relocation_index++];
    t->vtable->move_key((uint8_t *)candidate.keys + slot * t->vtable->key_size,
                        key_at(t, i), t->context);
    t->vtable->move_value(
        (uint8_t *)candidate.values + slot * t->vtable->value_size,
        value_at(t, i), t->context);
  }
  CHTHOLLY_CONTAINER_FREE(relocation);
  CHTHOLLY_CONTAINER_FREE(t->metadata);
  CHTHOLLY_CONTAINER_FREE(t->keys);
  CHTHOLLY_CONTAINER_FREE(t->values);
  *t = candidate;
  const uint64_t old_generation = t->generation;
  const int32_t status = bump_generation(t);
  if (status == CHTHOLLY_NEXT_CONTAINER_V1_OK) {
    if (t->vtable->rehash_commit)
      t->vtable->rehash_commit(t->context, old_generation, t->generation);
  } else if (t->vtable->rehash_abort) {
    t->vtable->rehash_abort(t->context);
  }
  return status;
}

static int find_slot(const chtholly_next_container_v1_table *t,
                     const void *key, uint64_t *slot_out) {
  if (!t || !t->capacity)
    return 0;
  const uint64_t hash = t->vtable->hash(key, t->seed, t->context);
  for (uint64_t step = 0; step < t->capacity; ++step) {
    const uint64_t slot = probe(hash, step, t->capacity);
    if (meta(t)[slot] == EMPTY)
      return 0;
    if (meta(t)[slot] == OCCUPIED &&
        t->vtable->hash(key_at(t, slot), t->seed, t->context) == hash &&
        t->vtable->equal(key_at(t, slot), key, t->context)) {
      *slot_out = slot;
      return 1;
    }
  }
  return 0;
}

int32_t chtholly_next_container_v1_find(
    const chtholly_next_container_v1_table *t, const void *key,
    const void **value) {
  if (!t || !t->vtable || !key || !value)
    return CHTHOLLY_NEXT_CONTAINER_V1_INVALID_ARGUMENT;
  uint64_t slot;
  if (!find_slot(t, key, &slot))
    return CHTHOLLY_NEXT_CONTAINER_V1_NOT_FOUND;
  *value = value_at(t, slot);
  return CHTHOLLY_NEXT_CONTAINER_V1_OK;
}

int32_t chtholly_next_container_v1_insert(
    chtholly_next_container_v1_table *t, const void *key, const void *value,
    void *replaced_value, uint8_t *replaced) {
  if (!t || !t->vtable || !key || !value || !replaced)
    return CHTHOLLY_NEXT_CONTAINER_V1_INVALID_ARGUMENT;
  if (t->generation == UINT64_MAX)
    return CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW;
  *replaced = 0;
  if (!t->capacity || t->size > UINT64_MAX - t->deleted ||
      (t->capacity <= UINT64_MAX / 3 &&
       t->size + t->deleted >= t->capacity * 3 / 4) ||
      t->capacity > UINT64_MAX / 3) {
    if (t->capacity > UINT64_MAX / 2)
      return CHTHOLLY_NEXT_CONTAINER_V1_CAPACITY_OVERFLOW;
    const int32_t status = chtholly_next_container_v1_reserve(
        t, t->capacity ? t->capacity * 2 : 8);
    if (status != CHTHOLLY_NEXT_CONTAINER_V1_OK)
      return status;
  }
  const uint64_t hash = t->vtable->hash(key, t->seed, t->context);
  uint64_t tombstone = UINT64_MAX;
  for (uint64_t step = 0; step < t->capacity; ++step) {
    const uint64_t slot = probe(hash, step, t->capacity);
    if (meta(t)[slot] == EMPTY) {
      if (t->generation == UINT64_MAX)
        return CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW;
      const uint64_t destination = tombstone == UINT64_MAX ? slot : tombstone;
      meta(t)[destination] = OCCUPIED;
      t->vtable->move_key(key_at(t, destination), (void *)key, t->context);
      t->vtable->move_value(value_at(t, destination), (void *)value,
                            t->context);
      if (tombstone != UINT64_MAX)
        --t->deleted;
      ++t->size;
      return bump_generation(t);
    }
    if (meta(t)[slot] == DELETED) {
      if (tombstone == UINT64_MAX)
        tombstone = slot;
      continue;
    }
    if (t->vtable->hash(key_at(t, slot), t->seed, t->context) == hash &&
        t->vtable->equal(key_at(t, slot), key, t->context)) {
      if (replaced_value)
        t->vtable->move_value(replaced_value, value_at(t, slot), t->context);
      else
        t->vtable->drop_value(value_at(t, slot), t->context);
      t->vtable->move_value(value_at(t, slot), (void *)value, t->context);
      t->vtable->drop_key((void *)key, t->context);
      *replaced = 1;
      return bump_generation(t);
    }
  }
  if (tombstone != UINT64_MAX) {
    meta(t)[tombstone] = OCCUPIED;
    t->vtable->move_key(key_at(t, tombstone), (void *)key, t->context);
    t->vtable->move_value(value_at(t, tombstone), (void *)value, t->context);
    --t->deleted;
    ++t->size;
    return bump_generation(t);
  }
  return CHTHOLLY_NEXT_CONTAINER_V1_CAPACITY_OVERFLOW;
}

int32_t chtholly_next_container_v1_erase(
    chtholly_next_container_v1_table *t, const void *key, void *removed_value,
    uint8_t *removed) {
  if (!t || !t->vtable || !key || !removed)
    return CHTHOLLY_NEXT_CONTAINER_V1_INVALID_ARGUMENT;
  if (t->generation == UINT64_MAX)
    return CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW;
  *removed = 0;
  uint64_t slot;
  if (!find_slot(t, key, &slot))
    return CHTHOLLY_NEXT_CONTAINER_V1_NOT_FOUND;
  if (removed_value)
    t->vtable->move_value(removed_value, value_at(t, slot), t->context);
  else
    t->vtable->drop_value(value_at(t, slot), t->context);
  t->vtable->drop_key(key_at(t, slot), t->context);
  meta(t)[slot] = DELETED;
  --t->size;
  ++t->deleted;
  *removed = 1;
  return bump_generation(t);
}

int32_t chtholly_next_container_v1_clear(
    chtholly_next_container_v1_table *t) {
  if (!t || !t->vtable)
    return CHTHOLLY_NEXT_CONTAINER_V1_INVALID_ARGUMENT;
  if (t->generation == UINT64_MAX)
    return CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW;
  drop_slots(t);
  if (t->metadata)
    memset(t->metadata, EMPTY, (size_t)t->capacity);
  t->size = 0;
  t->deleted = 0;
  return bump_generation(t);
}

int32_t chtholly_next_container_v1_borrow_is_valid(
    const chtholly_next_container_v1_table *t, uint64_t generation) {
  if (!t)
    return CHTHOLLY_NEXT_CONTAINER_V1_INVALID_ARGUMENT;
  return t->generation == generation ? CHTHOLLY_NEXT_CONTAINER_V1_OK
                                      : CHTHOLLY_NEXT_CONTAINER_V1_NOT_FOUND;
}
