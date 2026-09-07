#include "chtholly/next_container_v1.h"

#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
std::uint64_t hash_value(const void *object, std::uint64_t seed, void *) {
  auto value = *static_cast<const std::uint64_t *>(object) ^ seed;
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
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

int main(int argc, char **argv) {
  std::setlocale(LC_NUMERIC, "C");
  const std::uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10)
                                      : 10000;
  chtholly_next_container_v1_vtable vtable{};
  vtable.magic = CHTHOLLY_NEXT_CONTAINER_V1_MAGIC;
  vtable.abi_version = CHTHOLLY_NEXT_CONTAINER_V1_ABI;
  vtable.semantic_epoch = 25;
  vtable.target_pointer_bits = sizeof(void *) * 8;
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
  if (chtholly_next_container_v1_init(&table, &vtable, nullptr, 17) !=
      CHTHOLLY_NEXT_CONTAINER_V1_OK)
    return 2;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t key = i;
    std::uint64_t value = i * 3;
    std::uint8_t replaced = 0;
    const auto status = chtholly_next_container_v1_insert(
        &table, &key, &value, nullptr, &replaced);
    if (status != CHTHOLLY_NEXT_CONTAINER_V1_OK || replaced)
      return 3;
  }
  std::uint64_t hits = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t key = i;
    const void *value = nullptr;
    if (chtholly_next_container_v1_find(&table, &key, &value) ==
        CHTHOLLY_NEXT_CONTAINER_V1_OK)
      ++hits;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start);
  const auto operations = count * 2;
  std::printf(
      "{\"schema\":\"chtholly.container-benchmark.v1\",\"container\":\"HashMap\","
      "\"representation\":\"scalar\",\"n\":%llu,\"hits\":%llu,"
      "\"capacity\":%llu,\"generation\":%llu,\"operations\":%llu,"
      "\"elapsed_ns\":%llu,\"ns_per_op\":%.3f}\n",
      static_cast<unsigned long long>(count),
      static_cast<unsigned long long>(hits),
      static_cast<unsigned long long>(
          chtholly_next_container_v1_capacity(&table)),
      static_cast<unsigned long long>(
          chtholly_next_container_v1_generation(&table)),
      static_cast<unsigned long long>(operations),
      static_cast<unsigned long long>(elapsed.count()),
      operations ? static_cast<double>(elapsed.count()) / operations : 0.0);
  chtholly_next_container_v1_destroy(&table);
  return hits == count ? 0 : 4;
}
