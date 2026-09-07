#include "test_check.h"
#include <string_view>

int main(int argc, char **argv) {
  int evaluations = 0;
  CHTHOLLY_TEST_CHECK(++evaluations == 1);
  if (evaluations != 1)
    return 2;
  if (argc == 2 && std::string_view(argv[1]) == "--fail")
    CHTHOLLY_TEST_CHECK(false);
  return 0;
}
