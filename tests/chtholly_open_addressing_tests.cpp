#include "chtholly/Support/OpenAddressingHashTable.h"

#include "test_check.h"
#include <cstdint>
#include <optional>

struct Key {
  std::uint64_t value = 0;
  friend bool operator==(const Key &, const Key &) = default;
};

struct ConstantHash {
  std::size_t operator()(const Key &) const noexcept { return 7; }
};

int main() {
  using Table = chtholly::support::OpenAddressingHashTable<Key, std::uint64_t,
                                                            ConstantHash>;
  Table table(19);
  std::optional<std::uint64_t> replaced;
  for (std::uint64_t value = 0; value != 32; ++value) {
    CHTHOLLY_TEST_CHECK(table.insert(Key{value}, value * 3, replaced) == Table::Status::Ok);
    CHTHOLLY_TEST_CHECK(!replaced.has_value());
  }
  CHTHOLLY_TEST_CHECK(table.size() == 32);
  CHTHOLLY_TEST_CHECK(table.find(Key{11}) && *table.find(Key{11}) == 33);
  CHTHOLLY_TEST_CHECK(table.insert(Key{11}, 99, replaced) == Table::Status::Ok);
  CHTHOLLY_TEST_CHECK(replaced && *replaced == 33);
  std::optional<std::uint64_t> removed;
  CHTHOLLY_TEST_CHECK(table.erase(Key{11}, removed) && removed && *removed == 99);
  CHTHOLLY_TEST_CHECK(table.find(Key{11}) == nullptr);
  CHTHOLLY_TEST_CHECK(table.insert(Key{11}, 111, replaced) == Table::Status::Ok);
  CHTHOLLY_TEST_CHECK(table.find(Key{11}) && *table.find(Key{11}) == 111);
  table.clear();
  CHTHOLLY_TEST_CHECK(table.size() == 0 && table.find(Key{1}) == nullptr);
  return 0;
}
