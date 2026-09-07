#include "fuzz/chtholly_source_fuzz.h"

#include "test_check.h"
#include <cstdint>
#include <string_view>
#include <string>
using namespace std::literals;

int main() {
  constexpr std::string_view Inputs[] = {
      "\0module main; fn main(): i32 { return 0; }"sv,
      "\1foreach (let item in move iterator) { break; }",
      "\2value +",
      "\0module broken; fn nested<T>(value: T): T { return move value;"sv,
  };
  for (const auto input : Inputs)
    CHTHOLLY_TEST_CHECK(fuzzNextSource(reinterpret_cast<const std::uint8_t *>(input.data()),
                          input.size()) == 0);
  for (unsigned version = 0; version <= 10; ++version) {
    std::string input(1, static_cast<char>(version * 3));
    input += "module main; fn main(): i32 { return 0; }";
    CHTHOLLY_TEST_CHECK(fuzzNextSource(
        reinterpret_cast<const std::uint8_t *>(input.data()), input.size()) == 0);
  }
  return 0;
}
