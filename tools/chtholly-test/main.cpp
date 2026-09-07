#include "chtholly/Testing/Test.h"

CHTHOLLY_TEST(framework_smoke, "framework") {
  CHTHOLLY_EXPECT(true);
  return 0;
}

int main(int argc, char **argv) { return chtholly::testing::run(argc, argv); }
