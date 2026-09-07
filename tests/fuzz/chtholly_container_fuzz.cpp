#include "chtholly/next_container_v1.h"

#include <cstdint>
#include <cstdlib>

namespace {
std::uint64_t hash_value(const void *object, std::uint64_t seed, void *) {
  return *static_cast<const std::uint64_t *>(object) ^ seed;
}
int equal_value(const void *left, const void *right, void *) {
  return *static_cast<const std::uint64_t *>(left) ==
         *static_cast<const std::uint64_t *>(right);
}
void move_value(void *destination, void *source, void *) {
  *static_cast<std::uint64_t *>(destination) =
      *static_cast<const std::uint64_t *>(source);
  *static_cast<std::uint64_t *>(source) = 0;
}
void drop_value(void *, void *) {}
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (!data || size == 0 || size > 4096)
    return 0;
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
  vtable.hash = hash_value;
  vtable.equal = equal_value;
  vtable.move_key = move_value;
  vtable.move_value = move_value;
  vtable.drop_key = drop_value;
  vtable.drop_value = drop_value;
  chtholly_next_container_v1_table table{};
  if (chtholly_next_container_v1_init(&table, &vtable, nullptr, data[0]) !=
      CHTHOLLY_NEXT_CONTAINER_V1_OK)
    return 0;
#if defined(CHTHOLLY_RUNTIME_TESTING)
  chtholly_next_container_v1_testing_allocator_reset();
  if (data[0] & 1)
    chtholly_next_container_v1_testing_allocator_fail_after(data[0] % 8 + 1);
#endif
  for (std::size_t i = 1; i < size; ++i) {
    std::uint64_t key = data[i] % 32;
    std::uint64_t value = key ^ UINT64_C(0x9e3779b9);
    std::uint8_t replaced = 0;
    switch (data[i] % 4) {
    case 0:
      (void)chtholly_next_container_v1_insert(&table, &key, &value, nullptr,
                                              &replaced);
      break;
    case 1: {
      const void *found = nullptr;
      (void)chtholly_next_container_v1_find(&table, &key, &found);
      break;
    }
    case 2: {
      std::uint8_t removed = 0;
      (void)chtholly_next_container_v1_erase(&table, &key, nullptr, &removed);
      break;
    }
    default:
      (void)chtholly_next_container_v1_clear(&table);
      break;
    }
    if (chtholly_next_container_v1_size(&table) >
        chtholly_next_container_v1_capacity(&table))
      std::abort();
  }
  chtholly_next_container_v1_destroy(&table);
  return 0;
}
