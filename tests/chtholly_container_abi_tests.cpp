#include "chtholly/next_container_v1.h"

#include <cstdint>
#include <cstring>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      return __LINE__;                                                         \
  } while (false)

namespace {
struct Counters {
  std::uint64_t key_moves = 0;
  std::uint64_t value_moves = 0;
  std::uint64_t rehash_key_moves = 0;
  std::uint64_t rehash_value_moves = 0;
  std::uint64_t key_drops = 0;
  std::uint64_t value_drops = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t rehash_begins = 0;
  std::uint64_t rehash_commits = 0;
  std::uint64_t rehash_aborts = 0;
  bool in_rehash = false;
  bool fail_rehash = false;
};

std::uint64_t hash_value(const void *object, std::uint64_t seed, void *) {
  return *static_cast<const std::uint64_t *>(object) ^ seed;
}
int equal_value(const void *left, const void *right, void *) {
  return *static_cast<const std::uint64_t *>(left) ==
         *static_cast<const std::uint64_t *>(right);
}
void move_key(void *destination, void *source, void *context) {
  auto *counters = static_cast<Counters *>(context);
  ++counters->key_moves;
  if (counters->in_rehash)
    ++counters->rehash_key_moves;
  *static_cast<std::uint64_t *>(destination) =
      *static_cast<const std::uint64_t *>(source);
  *static_cast<std::uint64_t *>(source) = 0;
}
void move_value(void *destination, void *source, void *context) {
  auto *counters = static_cast<Counters *>(context);
  ++counters->value_moves;
  if (counters->in_rehash)
    ++counters->rehash_value_moves;
  *static_cast<std::uint64_t *>(destination) =
      *static_cast<const std::uint64_t *>(source);
  *static_cast<std::uint64_t *>(source) = 0;
}
void drop_key(void *, void *context) {
  ++static_cast<Counters *>(context)->key_drops;
}
void drop_value(void *, void *context) {
  ++static_cast<Counters *>(context)->value_drops;
}
void invalidate(void *context, std::uint64_t, std::uint64_t) {
  ++static_cast<Counters *>(context)->invalidations;
}
int rehash_begin(void *context, std::uint64_t, std::uint64_t) {
  auto *counters = static_cast<Counters *>(context);
  ++counters->rehash_begins;
  counters->in_rehash = true;
  return counters->fail_rehash ? CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY
                               : CHTHOLLY_NEXT_CONTAINER_V1_OK;
}
void rehash_commit(void *context, std::uint64_t, std::uint64_t) {
  auto *counters = static_cast<Counters *>(context);
  ++counters->rehash_commits;
  counters->in_rehash = false;
}
void rehash_abort(void *context) {
  auto *counters = static_cast<Counters *>(context);
  ++counters->rehash_aborts;
  counters->in_rehash = false;
}
} // namespace

int main() {
  Counters counters;
  chtholly_next_container_v1_vtable vtable{};
  vtable.magic = CHTHOLLY_NEXT_CONTAINER_V1_MAGIC;
  vtable.abi_version = CHTHOLLY_NEXT_CONTAINER_V1_ABI;
  vtable.semantic_epoch = 25;
  vtable.target_pointer_bits = 64;
  vtable.key_size = sizeof(std::uint64_t);
  vtable.key_alignment = alignof(std::uint64_t);
  vtable.value_size = sizeof(std::uint64_t);
  vtable.value_alignment = alignof(std::uint64_t);
  vtable.key_type_fingerprint[0] = 1;
  vtable.value_type_fingerprint[0] = 2;
  vtable.layout_fingerprint[0] = 3;
  vtable.context = &counters;
  vtable.hash = hash_value;
  vtable.equal = equal_value;
  vtable.move_key = move_key;
  vtable.move_value = move_value;
  vtable.drop_key = drop_key;
  vtable.drop_value = drop_value;
  vtable.borrow_invalidate = invalidate;
  vtable.rehash_begin = rehash_begin;
  vtable.rehash_commit = rehash_commit;
  vtable.rehash_abort = rehash_abort;
  CHECK(chtholly_next_container_v1_validate_vtable(&vtable, 25, 64) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(chtholly_next_container_v1_validate_vtable(&vtable, 24, 64) ==
        CHTHOLLY_NEXT_CONTAINER_V1_INVALID_DESCRIPTOR);
  CHECK(chtholly_next_container_v1_validate_vtable(&vtable, 25, 32) ==
        CHTHOLLY_NEXT_CONTAINER_V1_INVALID_DESCRIPTOR);
  auto invalid_alignment = vtable;
  invalid_alignment.key_alignment = 3;
  CHECK(chtholly_next_container_v1_validate_vtable(&invalid_alignment, 25, 64) ==
        CHTHOLLY_NEXT_CONTAINER_V1_INVALID_DESCRIPTOR);

  chtholly_next_container_v1_table table{};
  CHECK(chtholly_next_container_v1_init(&table, &vtable, &counters, 17) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  for (std::uint64_t i = 1; i != 64; ++i) {
    std::uint64_t key = i;
    std::uint64_t value = i * 3;
    std::uint8_t replaced = 0;
    CHECK(chtholly_next_container_v1_insert(&table, &key, &value, nullptr,
                                            &replaced) ==
          CHTHOLLY_NEXT_CONTAINER_V1_OK);
    CHECK(!replaced);
  }
  CHECK(chtholly_next_container_v1_size(&table) == 63);
  const auto insertion_generation =
      chtholly_next_container_v1_generation(&table);
  std::uint64_t extra_key = 1001;
  std::uint64_t extra_value = 3003;
  std::uint8_t extra_replaced = 0;
  CHECK(chtholly_next_container_v1_insert(&table, &extra_key, &extra_value,
                                          nullptr, &extra_replaced) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(chtholly_next_container_v1_borrow_is_valid(
            &table, insertion_generation) == CHTHOLLY_NEXT_CONTAINER_V1_NOT_FOUND);
  CHECK(chtholly_next_container_v1_borrow_is_valid(
            &table, insertion_generation + 1) == CHTHOLLY_NEXT_CONTAINER_V1_OK);
  const auto old_capacity = chtholly_next_container_v1_capacity(&table);
  const auto pre_reserve_generation =
      chtholly_next_container_v1_generation(&table);
  counters.fail_rehash = true;
  CHECK(chtholly_next_container_v1_reserve(&table, old_capacity * 2) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY);
  CHECK(chtholly_next_container_v1_capacity(&table) == old_capacity);
  CHECK(chtholly_next_container_v1_size(&table) == 64);
  CHECK(chtholly_next_container_v1_borrow_is_valid(
            &table, pre_reserve_generation) == CHTHOLLY_NEXT_CONTAINER_V1_OK);
  counters.fail_rehash = false;
  const void *found = nullptr;
  std::uint64_t key = 21;
  CHECK(chtholly_next_container_v1_find(&table, &key, &found) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(*static_cast<const std::uint64_t *>(found) == 63);
  std::uint64_t replacement = 999;
  std::uint64_t old_value = 0;
  std::uint8_t replaced = 0;
  CHECK(chtholly_next_container_v1_insert(&table, &key, &replacement,
                                          &old_value, &replaced) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(replaced && old_value == 63);
  std::uint8_t removed = 0;
  std::uint64_t removed_value = 0;
  CHECK(chtholly_next_container_v1_erase(&table, &key, &removed_value,
                                         &removed) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(removed && removed_value == 999);
  const auto generation = chtholly_next_container_v1_generation(&table);
  CHECK(chtholly_next_container_v1_borrow_is_valid(
            &table, pre_reserve_generation) != CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(chtholly_next_container_v1_borrow_is_valid(&table, generation) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(chtholly_next_container_v1_borrow_is_valid(&table, generation - 1) ==
        CHTHOLLY_NEXT_CONTAINER_V1_NOT_FOUND);
  CHECK(chtholly_next_container_v1_clear(&table) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(chtholly_next_container_v1_size(&table) == 0);
  chtholly_next_container_v1_destroy(&table);
  CHECK(counters.invalidations > 0);
  CHECK(counters.rehash_begins > 0 &&
        counters.rehash_commits + counters.rehash_aborts ==
            counters.rehash_begins &&
        counters.rehash_aborts == 1);
  CHECK(counters.key_drops == 64 + 1);
  CHECK(counters.value_drops == 63);
  CHECK(counters.key_moves == 64 + counters.rehash_key_moves);
  CHECK(counters.value_moves == 67 + counters.rehash_value_moves);
  std::int32_t create_status = -1;
  auto *created = chtholly_next_container_v1_create(&vtable, &counters, 23,
                                                    &create_status);
  CHECK(created != nullptr && create_status == CHTHOLLY_NEXT_CONTAINER_V1_OK);
  chtholly_next_container_v1_destroy(created);

#if defined(CHTHOLLY_RUNTIME_TESTING)
  for (std::uint64_t fail_point = 1; fail_point <= 1; ++fail_point) {
    chtholly_next_container_v1_testing_allocator_reset();
    chtholly_next_container_v1_testing_allocator_fail_after(fail_point);
    std::int32_t failed_create_status = CHTHOLLY_NEXT_CONTAINER_V1_OK;
    CHECK(chtholly_next_container_v1_create(&vtable, &counters, 29,
                                            &failed_create_status) == nullptr);
    CHECK(failed_create_status == CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY);
    CHECK(chtholly_next_container_v1_testing_allocator_failures() == 1);
  }
  /* Exercise every allocation point in a populated rehash.  A failed
     allocation must leave the old table and generation untouched. */
  Counters fault_counters;
  vtable.context = &fault_counters;
  chtholly_next_container_v1_table fault_table{};
  CHECK(chtholly_next_container_v1_init(&fault_table, &vtable,
                                        &fault_counters, 31) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  for (std::uint64_t i = 1; i <= 12; ++i) {
    std::uint64_t fault_key = i;
    std::uint64_t fault_value = i + 100;
    std::uint8_t fault_replaced = 0;
    CHECK(chtholly_next_container_v1_insert(
              &fault_table, &fault_key, &fault_value, nullptr,
              &fault_replaced) == CHTHOLLY_NEXT_CONTAINER_V1_OK);
  }
  const auto fault_capacity =
      chtholly_next_container_v1_capacity(&fault_table);
  const auto fault_size = chtholly_next_container_v1_size(&fault_table);
  const auto fault_generation =
      chtholly_next_container_v1_generation(&fault_table);
  const auto pre_fault_aborts = fault_counters.rehash_aborts;
  const auto pre_fault_commits = fault_counters.rehash_commits;
  for (std::uint64_t fail_point = 1; fail_point <= 4; ++fail_point) {
    chtholly_next_container_v1_testing_allocator_reset();
    chtholly_next_container_v1_testing_allocator_fail_after(fail_point);
    CHECK(chtholly_next_container_v1_reserve(&fault_table,
                                             fault_capacity * 2) ==
          CHTHOLLY_NEXT_CONTAINER_V1_OUT_OF_MEMORY);
    CHECK(chtholly_next_container_v1_testing_allocator_failures() == 1);
    CHECK(chtholly_next_container_v1_capacity(&fault_table) == fault_capacity);
    CHECK(chtholly_next_container_v1_size(&fault_table) == fault_size);
    CHECK(chtholly_next_container_v1_generation(&fault_table) ==
          fault_generation);
    std::uint64_t lookup_key = 7;
    const void *lookup_value = nullptr;
    CHECK(chtholly_next_container_v1_find(&fault_table, &lookup_key,
                                          &lookup_value) ==
          CHTHOLLY_NEXT_CONTAINER_V1_OK);
    CHECK(*static_cast<const std::uint64_t *>(lookup_value) == 107);
  }
  chtholly_next_container_v1_testing_allocator_reset();
  CHECK(chtholly_next_container_v1_reserve(&fault_table, fault_capacity * 2) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  CHECK(chtholly_next_container_v1_generation(&fault_table) ==
        fault_generation + 1);
  CHECK(fault_counters.rehash_aborts == pre_fault_aborts + 1);
  CHECK(fault_counters.rehash_commits == pre_fault_commits + 1);
  const auto frees_before_destroy =
      chtholly_next_container_v1_testing_allocator_frees();
  CHECK(frees_before_destroy == 4);
  chtholly_next_container_v1_destroy(&fault_table);
  CHECK(chtholly_next_container_v1_testing_allocator_frees() ==
        frees_before_destroy + 3);

  chtholly_next_container_v1_table overflow_table{};
  CHECK(chtholly_next_container_v1_init(&overflow_table, &vtable,
                                        &fault_counters, 37) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK);
  overflow_table.generation = UINT64_MAX;
  std::uint64_t overflow_key = 1;
  std::uint64_t overflow_value = 2;
  std::uint8_t overflow_replaced = 0;
  CHECK(chtholly_next_container_v1_insert(
              &overflow_table, &overflow_key, &overflow_value, nullptr,
              &overflow_replaced) ==
        CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW);
  CHECK(chtholly_next_container_v1_reserve(&overflow_table, 8) ==
        CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW);
  CHECK(chtholly_next_container_v1_clear(&overflow_table) ==
        CHTHOLLY_NEXT_CONTAINER_V1_GENERATION_OVERFLOW);
  chtholly_next_container_v1_destroy(&overflow_table);
#endif
  return 0;
}
