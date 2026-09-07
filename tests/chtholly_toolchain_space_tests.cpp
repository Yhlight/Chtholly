#include "ToolchainSpace.h"

#include "test_check.h"

int main() {
  const auto estimate =
      chtholly::toolchain_internal::estimateToolchainInstallSpace(4096, 512,
                                                                   1024);
  CHTHOLLY_TEST_CHECK(estimate.payload_bytes == 4096);
  CHTHOLLY_TEST_CHECK(estimate.index_bytes == 512);
  CHTHOLLY_TEST_CHECK(estimate.required_bytes == 4608);
  CHTHOLLY_TEST_CHECK(!estimate.sufficient);
  CHTHOLLY_TEST_CHECK(estimate.available_bytes == 1024);
  return 0;
}
