#include "chtholly/component_loader_v1.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "ABI-2 rejection check failed at line %d: %s\n", line,
                 expression);
    std::abort();
  }
}
#define CHECK(condition) check((condition), #condition, __LINE__)
} // namespace

int main(int argc, char **argv) {
  CHECK(argc == 2);
  chtholly_component_requirement_v1 requirement{};
  requirement.struct_size = sizeof(requirement);
  requirement.abi_epoch = CHTHOLLY_COMPONENT_ABI_EPOCH_V1;
  std::fill(std::begin(requirement.identity_digest),
            std::end(requirement.identity_digest), std::uint8_t{1});
  std::fill(std::begin(requirement.contract_digest),
            std::end(requirement.contract_digest), std::uint8_t{1});
  std::fill(std::begin(requirement.target_digest),
            std::end(requirement.target_digest), std::uint8_t{1});
  std::fill(std::begin(requirement.runtime_abi_digest),
            std::end(requirement.runtime_abi_digest), std::uint8_t{1});
  std::array<char, 256> diagnostic{};
  std::uint64_t diagnostic_size = 0;
  chtholly_component_module_v1 *module = nullptr;
  const auto status = chtholly_component_load_v1(
      argv[1], &requirement, &module, diagnostic.data(), diagnostic.size(),
      &diagnostic_size);
  CHECK(status == CHTHOLLY_COMPONENT_LOADER_ABI_MISMATCH_V1);
  CHECK(module == nullptr);
  CHECK(std::string_view(diagnostic.data(), diagnostic_size) ==
         "ABI-1 loader rejects ABI-2 component descriptor");
  return 0;
}
