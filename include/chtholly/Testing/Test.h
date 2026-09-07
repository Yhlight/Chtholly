#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::testing {

class Failure : public std::runtime_error {
 public:
  explicit Failure(std::string message) : std::runtime_error(std::move(message)) {}
};

void expect(bool condition, std::string_view expression, std::string_view file,
            int line);

struct TestCase {
  std::string name;
  std::string label;
  std::function<int()> body;
};

class Registry {
 public:
  static Registry &instance();
  void add(TestCase test);
  [[nodiscard]] const std::vector<TestCase> &tests() const { return tests_; }

 private:
  std::vector<TestCase> tests_;
};

class Registration {
 public:
  Registration(std::string name, std::string label, std::function<int()> body);
};

int run(int argc, char **argv);

} // namespace chtholly::testing

#define CHTHOLLY_TEST(name, label)                                             \
  static int chtholly_test_body_##name();                                      \
  static ::chtholly::testing::Registration chtholly_test_registration_##name(  \
      #name, label, chtholly_test_body_##name);                                \
  static int chtholly_test_body_##name()

#define CHTHOLLY_EXPECT(condition)                                             \
  ::chtholly::testing::expect(static_cast<bool>(condition), #condition,        \
                              __FILE__, __LINE__)
