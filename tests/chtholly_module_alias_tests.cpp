#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"

#include "test_check.h"
#include <iostream>
#include <string>

namespace {

using chtholly::compiler::CompilationRequest;
using chtholly::compiler::CompilationSession;
using chtholly::compiler::SourceInput;

CompilationSession makeV13Session(std::string package) {
  auto contract = chtholly::CurrentLanguageContract;
  contract.source = chtholly::FrozenV13LanguageVersion;
  return CompilationSession(
      chtholly_test::targetTriple, std::move(package), {},
      chtholly::compiler::defaultCompileToolchainFingerprint(), {}, contract);
}

void expectFailure(std::string name, std::string source) {
  auto session = makeV13Session(name);
  std::string error;
  CHTHOLLY_TEST_CHECK(session.addUnit(SourceInput(name + ".cns", std::move(source)))
             .hasValue());
  if (session.compile(error)) {
    std::cerr << "module alias fixture unexpectedly compiled: " << name << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(error.find("import") != std::string::npos);
}

} // namespace

int main() {
  auto provider = makeV13Session("alias-provider");
  CHTHOLLY_TEST_CHECK(provider
             .addUnit(SourceInput("provider.cns",
                                  R"cns(module provider;
pub fn answer(): i32 {
  return 42;
}
)cns"))
             .hasValue());
  std::string error;
  CHTHOLLY_TEST_CHECK(provider.compile(error));

  auto facade = makeV13Session("alias-facade");
  CHTHOLLY_TEST_CHECK(facade
             .addUnit(SourceInput("facade.cns",
                                  R"cns(module facade;
export import provider as p;

pub fn call(): i32 {
  return p::answer();
}
)cns"))
             .hasValue());
  CompilationRequest facade_request;
  facade_request.dependency_manifests = {&provider.packageManifest()};
  CHTHOLLY_TEST_CHECK(facade.compile(error, facade_request));

  const auto *facade_module = facade.packageManifest().findModule("facade");
  CHTHOLLY_TEST_CHECK(facade_module != nullptr);
  const auto *forwarded =
      facade_module->public_interface.findFunction("answer");
  CHTHOLLY_TEST_CHECK(forwarded != nullptr);
  CHTHOLLY_TEST_CHECK(forwarded->canonical_package == "alias-provider");
  CHTHOLLY_TEST_CHECK(forwarded->canonical_module == "provider");
  CHTHOLLY_TEST_CHECK(forwarded->canonical_name == "answer");

  auto consumer = makeV13Session("alias-consumer");
  const auto consumer_unit = consumer.addUnit(SourceInput("consumer.cns",
                                                          R"cns(module consumer;
import facade as f;

fn main(): i32 {
  return f::answer();
}
)cns"));
  CHTHOLLY_TEST_CHECK(consumer_unit.hasValue());
  CompilationRequest consumer_request;
  consumer_request.dependency_manifests = {&facade.packageManifest()};
  CHTHOLLY_TEST_CHECK(consumer.compile(error, consumer_request));
  CHTHOLLY_TEST_CHECK(consumer.unit(consumer_unit).printLLVM().find("declare") !=
         std::string::npos);

  expectFailure("duplicate-alias",
                R"cns(module duplicate_alias;
import first as p;
import second as p;
)cns");
  expectFailure("lookup-name-collision",
                R"cns(module lookup_name_collision;
import first;
import second as first;
)cns");

  auto collision = makeV13Session("alias-collision");
  CHTHOLLY_TEST_CHECK(collision.addUnit(SourceInput("provider.cns", "module provider;"))
             .hasValue());
  CHTHOLLY_TEST_CHECK(collision
             .addUnit(
                 SourceInput("collision.cns",
                             "module collision; import provider as provider;"))
             .hasValue());
  CHTHOLLY_TEST_CHECK(!collision.compile(error));
  CHTHOLLY_TEST_CHECK(error.find("import") != std::string::npos);
  return 0;
}
