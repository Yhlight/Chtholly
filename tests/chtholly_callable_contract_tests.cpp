#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"

#include "test_check.h"
#include <iostream>
#include <string>
#include <string_view>

namespace {

using chtholly::compiler::CompilationSession;
using chtholly::compiler::SourceInput;

void expectSuccess(std::string_view name, std::string_view source) {
  CompilationSession session(chtholly_test::targetTriple,
                             std::string("contract-") + std::string(name));
  std::string error;
  const auto unit_id = session.addUnit(
      SourceInput(std::string(name), std::string(source)));
  CHTHOLLY_TEST_CHECK(unit_id.hasValue());
  (void)unit_id;
  if (!session.compile(error)) {
    std::cerr << "contract fixture failed: " << name << "\n"
              << error << "\n";
    std::abort();
  }
}

void expectFailure(std::string_view name, std::string_view source) {
  CompilationSession session(chtholly_test::targetTriple,
                             std::string("contract-") + std::string(name));
  std::string error;
  const auto unit_id = session.addUnit(
      SourceInput(std::string(name), std::string(source)));
  CHTHOLLY_TEST_CHECK(unit_id.hasValue());
  if (session.compile(error)) {
    std::cerr << "contract fixture unexpectedly compiled: " << name << "\n";
    std::abort();
  }
}

} // namespace

int main() {
  expectSuccess(
      "bodyless-contract",
      R"cns(module contract_bodyless;
fn read(value: i32): i32 contract {
  reads value;
}
)cns");

  expectSuccess(
      "contract-merge",
      R"cns(module contract_merge;
fn read(value: i32): i32 contract {
  reads value;
}
fn read(value: i32): i32 contract {
  reads value;
}
)cns");

  expectFailure(
      "contract-mismatch",
      R"cns(module contract_mismatch;
fn read(value: i32&): i32& contract {
  borrows shared value;
  returns borrow value;
}
fn read(value: i32&): i32& contract {
  writes value;
}
)cns");

  expectFailure(
      "invalid-contract-entry",
      R"cns(module contract_invalid;
fn read(value: i32): i32 contract {
  borrows unknown value;
}
)cns");

  expectFailure(
      "initialize-is-not-chtholly-contract-effect",
      R"cns(module contract_initialize_boundary;
fn update(target: i32&): void contract {
  initialize target;
}
)cns");

  expectFailure(
      "contract-followed-by-body",
      R"cns(module contract_body;
fn read(value: i32): i32 contract {
  reads value;
} {
  return value;
}
)cns");

  expectFailure(
      "uncontracted-forward",
      R"cns(module uncontracted_forward;
fn read(value: i32): i32;
)cns");
  return 0;
}
