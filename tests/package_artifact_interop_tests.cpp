#include "chtholly/Driver/ArtifactStore.h"
#include "chtholly/Driver/CompilerInvocation.h"
#include "chtholly/Driver/CompilerBuildPlan.h"
#include "chtholly/Driver/CompilerPipeline.h"
#include "chtholly/Driver/PackageArtifactArchive.h"
#include "chtholly/Driver/PackageArtifactClosure.h"
#include "chtholly/Compiler/CompilationUnit.h"
#include "chtholly/Compiler/InteropArtifact.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include "test_check.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using chtholly::PackageArtifactArchiveInfo;
using chtholly::PackageArtifactManifest;
using chtholly::compiler::CompilationSession;
using chtholly::compiler::SourceInput;
using chtholly::compiler::interop::ArtifactBundle;
using chtholly::compiler::interop::ArtifactRegistry;
using chtholly::compiler::interop::ForeignCapability;
using chtholly::compiler::interop::ForeignOperationArtifact;
using chtholly::compiler::interop::ForeignOperationKind;

CompilationSession
makeV13Session(std::string package,
               std::string target = "x86_64-pc-windows-msvc") {
  auto contract = chtholly::CurrentLanguageContract;
  contract.source = chtholly::FrozenV13LanguageVersion;
  return CompilationSession(
      std::move(target), std::move(package), {},
      chtholly::compiler::defaultCompileToolchainFingerprint(), {}, contract);
}

std::string writeBinary(const fs::path &path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  CHTHOLLY_TEST_CHECK(output);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  CHTHOLLY_TEST_CHECK(output);
  return path.string();
}

std::string fixtureText(const fs::path &relative) {
  std::string error;
  const auto path =
      fs::path(CHTHOLLY_SOURCE_DIR) / "tests" / "fixtures" / relative;
  auto text = chtholly::readTextFile(path.string(), error);
  if (!text) {
    std::cerr << "read fixture failed: " << path.string() << ": " << error
              << "\n";
    std::abort();
  }
  return *text;
}

chtholly::compiler::CFFIReceiptIdentity
testCFFIIdentity(std::string target = "x86_64-pc-windows-msvc") {
  const auto digest = [](std::string_view value) {
    return chtholly::sha256Hex(value);
  };
  return {.target = std::move(target),
          .compiler_family = "msvc",
          .clang_version = digest("clang-version"),
          .libclang = digest("libclang"),
          .compiler = digest("compiler"),
          .compiler_version = digest("compiler-version"),
          .toolchain = digest("toolchain"),
          .sdk = digest("sdk"),
          .config = digest("config"),
          .headers = digest("headers"),
          .cfdl = digest("cfdl"),
          .probe = digest("probe"),
          .facts = digest("facts")};
}

std::uint64_t jsonNumber(std::string_view text, std::string_view key) {
  const auto marker = std::string("\"") + std::string(key) + "\"";
  const auto key_offset = text.find(marker);
  CHTHOLLY_TEST_CHECK(key_offset != std::string_view::npos);
  const auto colon = text.find(':', key_offset + marker.size());
  CHTHOLLY_TEST_CHECK(colon != std::string_view::npos);
  const auto first = text.find_first_not_of(" \t\r\n", colon + 1);
  CHTHOLLY_TEST_CHECK(first != std::string_view::npos);
  std::size_t length = 0;
  const auto value = std::stoull(std::string(text.substr(first)), &length);
  CHTHOLLY_TEST_CHECK(length != 0);
  return value;
}

std::uint64_t jsonObjectNumber(std::string_view text, std::string_view object,
                               std::string_view key) {
  const auto offset = text.find(object);
  CHTHOLLY_TEST_CHECK(offset != std::string_view::npos);
  return jsonNumber(text.substr(offset), key);
}

struct Fixture {
  fs::path root;
  fs::path package;
  fs::path manifest;
  fs::path interop;
  fs::path cffi_receipt;
  PackageArtifactManifest state;
  chtholly::compiler::interop::ArtifactReference reference;
};

Fixture makeFixture(std::string_view package_name = "interop-test") {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  Fixture fixture;
  fixture.root = fs::temp_directory_path() /
                 ("chtholly-package-interop-" + std::to_string(nonce));
  fixture.package = fixture.root / "package";
  fixture.manifest = fixture.package / "package.artifact";
  fixture.interop = fixture.package / "bindings.interop";
  fixture.cffi_receipt = fixture.package / "bindings.cffi-verify";
  fs::create_directories(fixture.package);

  ForeignOperationArtifact artifact;
  artifact.kind = ForeignOperationKind::Resource;
  artifact.capabilities.push_back(
      ForeignCapability{.path = "resource.release"});
  artifact.fingerprint = chtholly::compiler::StableFingerprint::fromCanonicalBytes(
      "package-interop-v1");
  std::string error;
  ArtifactRegistry source_registry;
  fixture.reference =
      source_registry.publish(package_name, "ffi", "release", artifact, error);
  CHTHOLLY_TEST_CHECK(!fixture.reference.canonical_package.empty());
  const auto object_path = fixture.package / "module.o";
  const auto component_path = fixture.package / "module.chcomponent";
  ArtifactBundle bundle;
  bundle.records.push_back({fixture.reference, artifact});
  if (!chtholly::compiler::interop::writeArtifactBundle(
          bundle, fixture.interop.string(), error)) {
    std::cerr << "write bundle failed: " << error << "\n";
    std::abort();
  }
  writeBinary(object_path, "object");
  auto cffi_identity = testCFFIIdentity();
  auto cffi_receipt = chtholly::compiler::renderCFFIReceipt(cffi_identity, error);
  CHTHOLLY_TEST_CHECK(!cffi_receipt.empty());
  writeBinary(fixture.cffi_receipt, cffi_receipt);
  chtholly::compiler::ComponentContractArtifact component_contract;
  component_contract.identity = "org.chtholly.tests.package-component";
  chtholly::compiler::ComponentExportArtifact component_export;
  component_export.canonical_name = "fixture::identity";
  component_export.parameters = {chtholly::compiler::ComponentValueKind::I32};
  component_export.result = chtholly::compiler::ComponentValueKind::I32;
  component_contract.exports.push_back(std::move(component_export));
  component_contract.canonicalize();
  const auto component_bytes = component_contract.encode(error);
  CHTHOLLY_TEST_CHECK(!component_bytes.empty());
  writeBinary(component_path, component_bytes);
  fixture.state.package_name = package_name;
  fixture.state.producer_compiler = "test";
  fixture.state.semantic_interface_format = "17";
  fixture.state.target.triple = "x86_64-pc-windows-msvc";
  fixture.state.target.pointer_width_bits = 64;
  fixture.state.runtime_abi = std::string(chtholly::HostedRuntimeAbiVersion);
  fixture.state.object = {chtholly::sha256File(object_path.string()).value(),
                          "module.o"};
  fixture.state.interop_bundle = {
      chtholly::sha256File(fixture.interop.string()).value(),
      "bindings.interop"};
  fixture.state.cffi_receipt = {
      std::move(cffi_identity),
      {chtholly::sha256File(fixture.cffi_receipt.string()).value(),
       "bindings.cffi-verify"}};
  fixture.state.component_abi_epoch = 1;
  fixture.state.component_identity = component_contract.identity;
  fixture.state.component_contract_digest =
      component_contract.contract_digest.hex();
  fixture.state.component_contract = {
      chtholly::sha256File(component_path.string()).value(),
      "module.chcomponent"};
  if (!chtholly::writePackageArtifactManifest(
          fixture.state, fixture.manifest.string(), error)) {
    std::cerr << "write manifest failed: " << error << "\n";
    std::abort();
  }
  return fixture;
}

void cleanup(const Fixture &fixture) {
  std::error_code error;
  fs::remove_all(fixture.root, error);
}

void manifest_and_archive_round_trip() {
  auto fixture = makeFixture();
  std::string error;
  auto loaded = chtholly::loadPackageArtifactManifest(
      fixture.manifest.string(), fixture.state.target,
      fixture.state.abi_version, error);
  CHTHOLLY_TEST_CHECK(loaded.has_value());
  CHTHOLLY_TEST_CHECK(loaded->interop_bundle.has_value());
  CHTHOLLY_TEST_CHECK(loaded->cffi_receipt.has_value());
  CHTHOLLY_TEST_CHECK(loaded->cffi_receipt->identity ==
         fixture.state.cffi_receipt->identity);
  CHTHOLLY_TEST_CHECK(loaded->component_contract.has_value());

  auto old_text = chtholly::readTextFile(fixture.manifest.string(), error);
  CHTHOLLY_TEST_CHECK(old_text.has_value());
  const auto old_header = old_text->find("chtholly-package-artifact-v15");
  CHTHOLLY_TEST_CHECK(old_header != std::string::npos);
  old_text->replace(old_header,
                    std::string("chtholly-package-artifact-v15").size(),
                    "chtholly-package-artifact-v14");
  CHTHOLLY_TEST_CHECK(!chtholly::parsePackageArtifactManifest(
      *old_text, fixture.manifest.string(), error));

  auto closure =
      chtholly::loadPackageArtifactClosure(fixture.manifest.string(), error);
  CHTHOLLY_TEST_CHECK(closure.has_value());
  CHTHOLLY_TEST_CHECK(closure->files.size() == 5);

  const auto archive = fixture.root / "package.zip";
  auto packed = chtholly::packPackageArtifactArchive(fixture.manifest.string(),
                                                     archive.string(), error);
  CHTHOLLY_TEST_CHECK(packed.has_value());
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(packed->files, [](const auto &file) {
    return file.relative_path == "package/bindings.interop";
  }));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(packed->files, [](const auto &file) {
    return file.relative_path == "package/module.chcomponent";
  }));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(packed->files, [](const auto &file) {
    return file.relative_path == "package/bindings.cffi-verify";
  }));

  auto inspected =
      chtholly::inspectPackageArtifactArchive(archive.string(), error);
  CHTHOLLY_TEST_CHECK(inspected.has_value());
  const auto extracted = fixture.root / "extracted";
  PackageArtifactArchiveInfo extracted_info;
  CHTHOLLY_TEST_CHECK(chtholly::extractPackageArtifactArchive(
      archive.string(), extracted.string(), extracted_info, error));
  const auto extracted_manifest =
      extracted / "tree" / "package" / "package.artifact";
  auto extracted_text =
      chtholly::readTextFile(extracted_manifest.string(), error);
  if (!extracted_text) {
    std::cerr << "read extracted manifest failed: " << error << "\n";
    std::abort();
  }
  auto extracted_state = chtholly::parsePackageArtifactManifest(
      *extracted_text, extracted_manifest.string(), error);
  if (!extracted_state) {
    std::cerr << "parse extracted manifest failed: " << error
              << " path=" << extracted_manifest.string() << "\n";
    std::abort();
  }
  ArtifactRegistry restored;
  CHTHOLLY_TEST_CHECK(
      chtholly::loadPackageArtifactInterop(*extracted_state, restored, error));
  CHTHOLLY_TEST_CHECK(restored.resolve(fixture.reference) != nullptr);

  auto mismatched = fixture.state;
  mismatched.component_contract_digest = chtholly::sha256Hex("mismatch");
  const auto mismatched_manifest = fixture.package / "mismatched.artifact";
  CHTHOLLY_TEST_CHECK(chtholly::writePackageArtifactManifest(
      mismatched, mismatched_manifest.string(), error));
  CHTHOLLY_TEST_CHECK(!chtholly::loadPackageArtifactClosure(mismatched_manifest.string(),
                                               error));
  CHTHOLLY_TEST_CHECK(error.find("disagrees with CHNXCMP1") != std::string::npos);

  auto mismatched_cffi = fixture.state;
  mismatched_cffi.cffi_receipt->identity.facts =
      chtholly::sha256Hex("mismatched-facts");
  const auto mismatched_cffi_manifest =
      fixture.package / "mismatched-cffi.artifact";
  CHTHOLLY_TEST_CHECK(chtholly::writePackageArtifactManifest(
      mismatched_cffi, mismatched_cffi_manifest.string(), error));
  CHTHOLLY_TEST_CHECK(!chtholly::loadPackageArtifactManifest(
      mismatched_cffi_manifest.string(), fixture.state.target,
      fixture.state.abi_version, error));
  CHTHOLLY_TEST_CHECK(error.find("CFFI receipt identity mismatch") != std::string::npos);
  cleanup(fixture);
}

void session_loads_before_compile() {
  auto fixture = makeFixture();
  std::string error;
  CompilationSession session("x86_64-pc-windows-msvc", "interop-test");
  CHTHOLLY_TEST_CHECK(session.loadInteropBundle(fixture.interop.string(), "interop-test",
                                   error));
  CHTHOLLY_TEST_CHECK(session
             .addUnit(SourceInput("main.cns",
                                  "module main; fn main(): i32 { return 0; }"))
             .hasValue());
  CHTHOLLY_TEST_CHECK(session.compile(error));
  cleanup(fixture);
}

void tampered_and_mismatched_bundles_are_rejected() {
  auto fixture = makeFixture();
  std::string error;
  CompilationSession session("x86_64-pc-windows-msvc", "interop-test");
  CHTHOLLY_TEST_CHECK(!session.loadInteropBundle(fixture.interop.string(), "other-package",
                                    error));
  CHTHOLLY_TEST_CHECK(error.find("different package") != std::string::npos);

  std::fstream file(fixture.interop,
                    std::ios::in | std::ios::out | std::ios::binary);
  CHTHOLLY_TEST_CHECK(file);
  char first = 0;
  file.read(&first, 1);
  file.seekp(0);
  const char tampered = static_cast<char>(first ^ 0x01);
  file.write(&tampered, 1);
  file.close();
  auto rejected = chtholly::loadPackageArtifactManifest(
      fixture.manifest.string(), fixture.state.target,
      fixture.state.abi_version, error);
  CHTHOLLY_TEST_CHECK(!rejected.has_value());
  CHTHOLLY_TEST_CHECK(error.find("SHA-256 mismatch") != std::string::npos);
  cleanup(fixture);
}

void next_plan_resolves_declared_bundle() {
  auto fixture = makeFixture();
  const auto source = fixture.package / "main.cns";
  writeBinary(source, "module main; fn main(): i32 { return 0; }\n");
  writeBinary(
      fixture.package / "chtholly.toml",
      "[package]\nname = \"interop-test\"\nlanguage = \"1.3\"\n"
      "[build]\nentry = \"main.cns\"\ninterop_bundle = \"bindings.interop\"\n");
  chtholly::CompilerInvocation invocation;
  invocation.project_path = fixture.package.string();
#ifdef _WIN32
  invocation.target_triple = "x86_64-pc-windows-msvc";
#else
  invocation.target_triple = "x86_64-unknown-linux-gnu";
#endif
  std::string error;
  auto plan = chtholly::resolveNextBuildPlan(invocation, error);
  CHTHOLLY_TEST_CHECK(plan.has_value());
  const auto &package = plan->packages[plan->root_package];
  std::error_code path_error;
  CHTHOLLY_TEST_CHECK(std::filesystem::equivalent(package.interop_bundle_path,
                                      fixture.interop, path_error));
  CHTHOLLY_TEST_CHECK(!path_error);
  CHTHOLLY_TEST_CHECK(package.interop_bundle_digest ==
         chtholly::sha256File(fixture.interop.string()).value());
  cleanup(fixture);
}

void archive_dependency_enters_real_compile_session() {
  auto fixture = makeFixture("provider");
  std::string error;
  writeBinary(fixture.package / "entry.cns", "module provider_entry;\n");
  writeBinary(
      fixture.package / "library.cns",
      "module provider; pub fn identity<T>(value: T): T { return value; }\n");
  const std::string provider_binding =
      "module provider_binding; foreign type Session; "
      "foreign struct TypedefStruct { value: c_int; }; "
      "foreign fn poll(session: ref_mut Session) -> i32 where session invokes "
      "complete;\n";
  writeBinary(fixture.package / "provider_binding.cfdl", provider_binding);
  {
    CompilationSession provider_session(
        "x86_64-pc-windows-msvc", "provider", {},
        chtholly::compiler::defaultCompileToolchainFingerprint(), {},
        chtholly::CurrentLanguageContract, {},
        fixture.state.cffi_receipt->identity);
    CHTHOLLY_TEST_CHECK(provider_session
               .addUnit(SourceInput("provider_binding.cfdl", provider_binding),
                        chtholly::compiler::CompilationUnitKind::ForeignBinding)
               .hasValue());
    if (!provider_session.compile(error)) {
      std::cerr << "provider binding compile failed: " << error << "\n";
      std::abort();
    }
    const auto manifest_bytes =
        provider_session.packageManifest().encode(error);
    CHTHOLLY_TEST_CHECK(error.empty());
    auto manifest_round_trip =
        chtholly::compiler::CompilerPackageArtifactManifest::decode(manifest_bytes,
                                                            error);
    if (!manifest_round_trip) {
      std::cerr << "provider manifest round trip failed: " << error << "\n";
      std::abort();
    }
    CHTHOLLY_TEST_CHECK(manifest_round_trip->cffiIdentity().has_value());
    CHTHOLLY_TEST_CHECK(*manifest_round_trip->cffiIdentity() ==
           fixture.state.cffi_receipt->identity);
    CHTHOLLY_TEST_CHECK(manifest_round_trip->configurationFingerprint() ==
           provider_session.packageManifest().configurationFingerprint());
    CHTHOLLY_TEST_CHECK(
        provider_session.exportInteropBundle(fixture.interop.string(), error));
    fixture.state.interop_bundle->sha256 =
        chtholly::sha256File(fixture.interop.string()).value();
    CHTHOLLY_TEST_CHECK(chtholly::writePackageArtifactManifest(
        fixture.state, fixture.manifest.string(), error));
  }
  writeBinary(fixture.package / "chtholly.toml",
              "[package]\nname = \"provider\"\nlanguage = \"1.3\"\n"
              "[build]\nentry = \"entry.cns\"\nmodule_paths = [\".\"]\n");
  const auto archive = fixture.root / "provider.zip";
  auto packed = chtholly::packPackageArtifactArchive(fixture.manifest.string(),
                                                     archive.string(), error);
  CHTHOLLY_TEST_CHECK(packed.has_value());
  const auto archive_digest = chtholly::sha256File(archive.string()).value();

  const auto consumer = fixture.root / "consumer";
  fs::create_directories(consumer / "artifacts");
  fs::copy_file(archive, consumer / "artifacts" / "provider.zip");
  writeBinary(consumer / "main.cns",
              "module main; import provider; fn main(): i32 { return "
              "provider::identity(7) + provider::identity(8); }\n");
  writeBinary(consumer / "consumer_binding.cfdl",
              "module consumer_binding; import provider_binding; "
              "foreign fn observe(value: TypedefStruct) -> i32;\n");
  writeBinary(consumer / "chtholly.toml",
              "[package]\nname = \"consumer\"\nlanguage = \"1.3\"\n"
              "[target]\ntriple = \"x86_64-pc-windows-msvc\"\n"
              "[build]\nentry = \"main.cns\"\n"
              "[dependencies]\nprovider = { path = \"../package\", "
              "artifact = \"artifacts/provider.zip\", sha256 = \"" +
                  archive_digest + "\" }\n");
  chtholly::CompilerInvocation invocation;
  invocation.workflow = chtholly::DriverWorkflow::Build;
  invocation.action = chtholly::DriverAction::EmitLLVM;
  invocation.project_path = consumer.string();
  invocation.output_path = (fixture.root / "consumer.ll").string();
  invocation.cache_dir = (fixture.root / "cache").string();
  invocation.target_triple = "x86_64-pc-windows-msvc";
  invocation.suppress_lockfile_update = true;
  const auto metrics_first = (fixture.root / "metrics-first.json").string();
  const auto metrics_second = (fixture.root / "metrics-second.json").string();
  invocation.compiler_artifact_load_metrics_output_path = metrics_first;
  if (chtholly::runCompilerPipeline(invocation, error) != 0) {
    std::cerr << "archive compile failed: " << error << "\n";
    std::abort();
  }
  auto first_metrics = chtholly::readTextFile(metrics_first, error);
  if (!first_metrics) {
    std::cerr << "read first metrics failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(jsonNumber(*first_metrics, "attempts") == 1);
  CHTHOLLY_TEST_CHECK(jsonNumber(*first_metrics, "fresh-installs") +
             jsonNumber(*first_metrics, "closure-hits") ==
         1);
  invocation.compiler_artifact_load_metrics_output_path = metrics_second;
  if (chtholly::runCompilerPipeline(invocation, error) != 0) {
    std::cerr << "second archive compile failed: " << error << "\n";
    std::abort();
  }
  auto second_metrics = chtholly::readTextFile(metrics_second, error);
  if (!second_metrics) {
    std::cerr << "read second metrics failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(jsonNumber(*second_metrics, "attempts") == 1);
  CHTHOLLY_TEST_CHECK(jsonNumber(*second_metrics, "closure-hits") == 1);
  CHTHOLLY_TEST_CHECK(jsonNumber(*second_metrics, "fresh-installs") == 0);
  const auto specialization_requests = jsonObjectNumber(
      *second_metrics, "\"specialization-closure\"", "requests");
  CHTHOLLY_TEST_CHECK(specialization_requests > 0);
  CHTHOLLY_TEST_CHECK(jsonObjectNumber(*second_metrics, "\"specialization-closure\"",
                          "found") == specialization_requests);
  CHTHOLLY_TEST_CHECK(jsonObjectNumber(*second_metrics, "\"artifact-io\"", "found") > 0);
  CHTHOLLY_TEST_CHECK(jsonObjectNumber(*second_metrics, "\"executor\"", "completed") > 0);
  cleanup(fixture);
}

void stdlib_provider_generates_real_sidecar() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path() /
                    ("chtholly-stdlib-sidecar-" + std::to_string(nonce));
  const auto project = root / "project";
  const auto cache = root / "cache";
  fs::create_directories(project);
  writeBinary(project / "main.cns", "module main; import std::host; "
                                    "fn main(): i32 { return 0; }\n");
  writeBinary(project / "host_consumer.cfdl",
              "module host_consumer; import std::host; "
              "foreign fn observe(handle: ref_mut Handle) -> i32;\n");
  writeBinary(project / "chtholly.toml",
              "[package]\nname = \"host-consumer\"\nlanguage = \"1.3\"\n"
              "[target]\ntriple = \"x86_64-pc-windows-msvc\"\n"
              "[build]\nentry = \"main.cns\"\nmodule_paths = [\".\"]\n");

  chtholly::CompilerInvocation invocation;
  invocation.workflow = chtholly::DriverWorkflow::Compile;
  invocation.action = chtholly::DriverAction::EmitLLVM;
  invocation.project_path = project.string();
  invocation.output_path = (root / "host-consumer.ll").string();
  invocation.cache_dir = cache.string();
  invocation.target_triple = "x86_64-pc-windows-msvc";
  invocation.resource_dir =
      (fs::path(CHTHOLLY_BINARY_DIR) / "share" / "chtholly").string();
  invocation.suppress_lockfile_update = true;
  std::string error;
  if (chtholly::runCompilerPipeline(invocation, error) != 0) {
    std::cerr << "stdlib sidecar compile failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(chtholly::runCompilerPipeline(invocation, error) == 0);
  CHTHOLLY_TEST_CHECK(fs::is_regular_file(root / "host-consumer.ll"));
  bool found_std_host = false;
  // The cache contract fingerprint is intentionally versioned and may change
  // when compiler-owned semantic facts change. Discover the generated
  // interop sidecars beneath the cache root instead of baking an old
  // fingerprint into this integration test.
  for (fs::recursive_directory_iterator iterator(cache), end;
       iterator != end; ++iterator) {
    if (!iterator->is_regular_file() ||
        iterator->path().extension() != ".interop")
      continue;
    ArtifactBundle bundle;
    if (!chtholly::compiler::interop::readArtifactBundle(iterator->path().string(),
                                                     bundle, error)) {
      std::cerr << "read stdlib sidecar failed: " << iterator->path().string()
                << ": " << error << "\n";
      std::abort();
    }
    if (std::ranges::any_of(bundle.records, [](const auto &record) {
          return record.reference.canonical_package == "std" &&
                 record.reference.canonical_module == "std::host";
        }))
      found_std_host = true;
  }
  CHTHOLLY_TEST_CHECK(found_std_host);
  std::error_code cleanup_error;
  fs::remove_all(root, cleanup_error);
}

void stdlib_host_generated_executable_runs_provider_symbols() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path() /
                    ("chtholly-stdlib-host-e2e-" + std::to_string(nonce));
  fs::create_directories(root);

  chtholly::CompilerInvocation invocation;
  invocation.workflow = chtholly::DriverWorkflow::Run;
  invocation.project_path = (fs::path(CHTHOLLY_SOURCE_DIR) / "tests" /
                             "fixtures" / "chtholly-std-host-e2e")
                                .string();
  invocation.out_dir = (root / "bin").string();
  invocation.cache_dir = (root / "cache").string();
#ifdef _WIN32
  invocation.target_triple = "x86_64-pc-windows-msvc";
#else
  invocation.target_triple = "x86_64-unknown-linux-gnu";
#endif
  invocation.resource_dir =
      (fs::path(CHTHOLLY_BINARY_DIR) / "share" / "chtholly").string();
  invocation.suppress_lockfile_update = true;
  std::string error;
  CHTHOLLY_TEST_CHECK(chtholly::runCompilerPipeline(invocation, error) == 0);
  const auto executable = root / "bin" /
#ifdef _WIN32
                          "std-host-e2e.exe";
#else
                          "std-host-e2e";
#endif
  CHTHOLLY_TEST_CHECK(fs::is_regular_file(executable));

  std::error_code cleanup_error;
  fs::remove(fs::current_path() / "chtholly-host-e2e.tmp", cleanup_error);
  fs::remove_all(root, cleanup_error);
}

void artifact_store_install_race() {
  auto fixture = makeFixture();
  std::string error;
  const auto archive = fixture.root / "race.zip";
  auto packed = chtholly::packPackageArtifactArchive(fixture.manifest.string(),
                                                     archive.string(), error);
  CHTHOLLY_TEST_CHECK(packed.has_value());

  constexpr std::size_t thread_count = 8;
  const auto store_root = fixture.root / "race-store";
  std::vector<std::optional<PackageArtifactArchiveInfo>> results(thread_count);
  std::vector<chtholly::ArtifactStoreInstallObservation> observations(
      thread_count);
  std::vector<std::string> errors(thread_count);
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    workers.emplace_back([&, index] {
      chtholly::ArtifactStore store(store_root.string());
      results[index] =
          store.install(archive.string(), errors[index], &observations[index]);
    });
  }
  for (auto &worker : workers)
    worker.join();

  std::size_t fresh_installs = 0;
  std::size_t closure_hits = 0;
  CHTHOLLY_TEST_CHECK(results.front().has_value());
  const auto expected_identity = results.front()->artifact_identity;
  const auto expected_closure = results.front()->closure_digest;
  for (std::size_t index = 0; index < thread_count; ++index) {
    CHTHOLLY_TEST_CHECK(errors[index].empty());
    CHTHOLLY_TEST_CHECK(results[index].has_value());
    CHTHOLLY_TEST_CHECK(results[index]->artifact_identity == expected_identity);
    CHTHOLLY_TEST_CHECK(results[index]->closure_digest == expected_closure);
    if (observations[index].closure_hit)
      ++closure_hits;
    else
      ++fresh_installs;
    CHTHOLLY_TEST_CHECK(observations[index].archive_bytes > 0);
  }
  CHTHOLLY_TEST_CHECK(fresh_installs == 1);
  CHTHOLLY_TEST_CHECK(closure_hits == thread_count - 1);

  const auto locator = chtholly::renderArtifactStoreLocator(
      {expected_identity, expected_closure});
  chtholly::ArtifactStore verifier(store_root.string());
  auto inspected = verifier.inspect(locator, error);
  CHTHOLLY_TEST_CHECK(inspected.has_value());
  auto manifest_path =
      verifier.filePath(locator, inspected->root_manifest_relative_path, error);
  CHTHOLLY_TEST_CHECK(manifest_path.has_value());
  CHTHOLLY_TEST_CHECK(chtholly::readTextFile(*manifest_path, error).has_value());
  cleanup(fixture);
}

void cfdl_source_to_import_and_llvm() {
  auto session = makeV13Session("trusted-cfdl",
#ifdef _WIN32
                                "x86_64-pc-windows-msvc"
#else
                                "x86_64-unknown-linux-gnu"
#endif
  );
  const auto binding = session.addUnit(
      SourceInput(
          "binding.cfdl",
          "module binding;\n"
          "foreign type Session: void* invalid null;\n"
          "foreign struct Pair { left: i32; right: i32; };\n"
          "foreign struct LongPair { left: c_long; right: c_long; };\n"
          "foreign enum Status: c_int { STATUS_OK = 0; STATUS_FAILED = -1; "
          "};\n"
          "foreign union Number { integer: c_long; real: f64; };\n"
          "foreign fn version() -> i32;\n"
          "foreign fn pair_make(value: out Pair) -> i32;\n"
          "foreign fn pair_sum(value: Pair) -> i32;\n"
          "foreign fn long_pair_make(value: out LongPair) -> i32;\n"
          "foreign fn long_pair_sum(value: LongPair) -> i32;\n"
          "foreign fn number_make(value: out Number) -> Status "
          "link \"c_number_make\" call c;\n"
          "foreign fn number_sum(value: Number) -> i32 "
          "link \"c_number_sum\" call c;\n"
          "foreign fn set_callback(callback: c_fn c(i32)->void) -> i32 "
          "link \"c_set_callback\" call c;\n"
          "foreign fn open() -> owned Session where result obliges close;\n"
          "foreign fn close(session: ref_mut Session) -> void where session "
          "discharges close;\n"),
      chtholly::compiler::CompilationUnitKind::ForeignBinding);
  const auto main = session.addUnit(SourceInput(
      "main.cns", "module main; import binding as api; "
                  "fn main(): i32 { unsafe { let pair: api::Pair; "
                  "if (api::pair_make(pair) != 0) { return -1; } "
                  "let long_pair: api::LongPair; "
                  "if (api::long_pair_make(long_pair) != 0) { return -2; } "
                  "let status = api::STATUS_OK; let number: api::Number; "
                  "api::number_make(number); "
                  "return api::pair_sum(pair) + api::long_pair_sum(long_pair) "
                  "+ api::number_sum(number) + api::version(); } }\n"));
  CHTHOLLY_TEST_CHECK(binding.hasValue() && main.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << "trusted CFDL compile failed: " << error << "\n";
    std::abort();
  }
  const auto &binding_unit = session.unit(binding);
  CHTHOLLY_TEST_CHECK(binding_unit.publicInterface() != nullptr);
  const auto public_text = binding_unit.printPublicInterface();
  CHTHOLLY_TEST_CHECK(public_text.find("Session") != std::string::npos);
  CHTHOLLY_TEST_CHECK(public_text.find("open") != std::string::npos);
  CHTHOLLY_TEST_CHECK(public_text.find("Pair") != std::string::npos);
  const auto status_ok = std::ranges::find(
      binding_unit.publicInterface()->valueArtifacts(), "STATUS_OK",
      &chtholly::compiler::PublicValueArtifact::name);
  CHTHOLLY_TEST_CHECK(status_ok != binding_unit.publicInterface()->valueArtifacts().end());
  CHTHOLLY_TEST_CHECK(status_ok->type.kind == chtholly::compiler::PublicTypeKind::Nominal);
  CHTHOLLY_TEST_CHECK(status_ok->value.payload == 0);
  const auto *binding_module = session.packageManifest().findModule("binding");
  CHTHOLLY_TEST_CHECK(binding_module != nullptr);
  const auto set_callback = std::ranges::find(
      binding_module->public_interface.functions(), "set_callback",
      &chtholly::compiler::PublicFunctionArtifact::name);
  CHTHOLLY_TEST_CHECK(set_callback != binding_module->public_interface.functions().end() &&
         set_callback->parameters.size() == 1);
#ifdef _WIN32
  CHTHOLLY_TEST_CHECK(set_callback->parameters.front().foreign_calling_convention ==
         chtholly::compiler::ForeignCallingConvention::Win64);
#else
  CHTHOLLY_TEST_CHECK(set_callback->parameters.front().foreign_calling_convention ==
         chtholly::compiler::ForeignCallingConvention::SysV64);
#endif
  const auto *binding_sem_ir = binding_unit.semIR();
  CHTHOLLY_TEST_CHECK(binding_sem_ir != nullptr);
  bool saw_open_artifact = false;
  for (std::uint32_t index = 0; index < binding_sem_ir->functionCount();
       ++index) {
    const auto &function =
        binding_sem_ir->function(chtholly::compiler::FunctionId(index));
    const auto name =
        binding_sem_ir->identifier(binding_sem_ir->name(function.name).text);
    if (name != "open")
      continue;
    const auto &declaration =
        binding_sem_ir->functionDeclaration(chtholly::compiler::FunctionId(index));
    CHTHOLLY_TEST_CHECK(declaration.interop_artifact.has_value());
    CHTHOLLY_TEST_CHECK(declaration.interop_artifact->canonical_package == "trusted-cfdl");
    CHTHOLLY_TEST_CHECK(declaration.interop_artifact->canonical_module == "binding");
    saw_open_artifact = true;
  }
  CHTHOLLY_TEST_CHECK(saw_open_artifact);
  const auto llvm = session.unit(main).printLLVM();
  CHTHOLLY_TEST_CHECK(!llvm.empty());
  CHTHOLLY_TEST_CHECK(llvm.find("@pair_sum(i64") != std::string::npos);
#ifdef _WIN32
  CHTHOLLY_TEST_CHECK(llvm.find("@long_pair_sum(i64)") != std::string::npos);
  CHTHOLLY_TEST_CHECK(llvm.find("@c_number_make") != std::string::npos);
  CHTHOLLY_TEST_CHECK(llvm.find("win64cc") != std::string::npos);
#else
  CHTHOLLY_TEST_CHECK(llvm.find("@long_pair_sum(i64, i64)") != std::string::npos);
  CHTHOLLY_TEST_CHECK(llvm.find("@c_number_make") != std::string::npos);
  CHTHOLLY_TEST_CHECK(llvm.find("x86_64_sysvcc") != std::string::npos);
#endif
}

void stdlib_cfdl_fixture_compiles() {
  auto session = makeV13Session("stdlib-cfdl");
  const auto binding = session.addUnit(
      SourceInput(
          "session.cfdl",
          "module std::session;\n"
          "foreign type Session: void* invalid null;\n"
          "foreign fn open() -> owned Session where result obliges close;\n"
          "foreign fn wait(session: ref_mut Session) -> void where session "
          "invokes complete;\n"
          "foreign fn close(session: ref_mut Session) -> void where session "
          "discharges close;\n"),
      chtholly::compiler::CompilationUnitKind::ForeignBinding);
  const auto main = session.addUnit(
      SourceInput("main.cns", "module demo; import std::session as session; "
                              "fn main(): i32 { return 0; }\n"));
  CHTHOLLY_TEST_CHECK(binding.hasValue() && main.hasValue());
  std::string error;
  CHTHOLLY_TEST_CHECK(session.compile(error));
  CHTHOLLY_TEST_CHECK(!session.unit(binding).printLLVM().empty());
}

void stdlib_host_cfdl_fixture_compiles() {
  CompilationSession session("x86_64-pc-windows-msvc", "std-host");
  const auto binding = session.addUnit(
      SourceInput(
          "host.cfdl",
          "module std::host;\n"
          "foreign type Handle: void* invalid null;\n"
          "foreign struct Instant { seconds: u64; nanoseconds: u32; reserved: "
          "u32; };\n"
          "foreign type TaskHandle: void* invalid null;\n"
          "foreign fn host_open(path: view void*, path_size: u64, handle: out "
          "Handle) -> i32 where handle obliges close;\n"
          "foreign fn host_read(handle: ref_mut Handle, buffer: view_mut "
          "void*, count: u64) -> i64;\n"
          "foreign fn host_write(handle: ref_mut Handle, buffer: view_mut "
          "void*, count: u64) -> i64;\n"
          "foreign fn host_close(handle: move Handle) -> i32 where handle "
          "discharges close;\n"
          "foreign fn monotonic_now(value: out Instant) -> i32;\n"
          "foreign fn task_spawn(entry: view const void*, task: out "
          "TaskHandle) -> i32 where task obliges task_join;\n"
          "foreign fn task_poll(task: ref_mut TaskHandle) -> i32 where task "
          "invokes complete, task invokes cancelled, task invokes wake;\n"
          "foreign fn task_cancel(task: ref_mut TaskHandle) -> i32 where task "
          "invokes cancelled;\n"
          "foreign fn task_wake(task: ref_mut TaskHandle) -> i32 where task "
          "invokes wake;\n"
          "foreign fn task_join(task: move TaskHandle) -> i32 where task "
          "discharges task_join, task_join requires quiescent;\n"),
      chtholly::compiler::CompilationUnitKind::ForeignBinding);
  CHTHOLLY_TEST_CHECK(binding.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << "std::host CFDL compile failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(!session.unit(binding).printLLVM().empty());
}

void qualified_cfdl_protocol_reference_resolves() {
  CompilationSession session("x86_64-pc-windows-msvc", "protocol-reference");
  const auto acquire = session.addUnit(
      SourceInput(
          "acquire.cfdl",
          "module acquire; foreign type Session: void* invalid null; "
          "foreign fn open() -> owned Session where result obliges close; "
          "foreign fn wait(session: ref_mut Session) -> void where session "
          "invokes complete;"),
      chtholly::compiler::CompilationUnitKind::ForeignBinding);
  const auto release = session.addUnit(
      SourceInput("release.cfdl",
                  "module release; import acquire; "
                  "foreign fn close(session: move void*) -> void "
                  "where session discharges acquire::close; "
                  "foreign fn observe(session: ref_mut void*) -> void "
                  "where session invokes acquire::wait::complete;"),
      chtholly::compiler::CompilationUnitKind::ForeignBinding);
  CHTHOLLY_TEST_CHECK(acquire.hasValue() && release.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << "qualified CFDL protocol compile failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(!session.unit(release).printLLVM().empty());
}

void imported_cfdl_nominal_type_materializes() {
  CompilationSession session("x86_64-pc-windows-msvc", "nominal-import");
  const auto provider = session.addUnit(
      SourceInput(
          "provider.cfdl",
          "module provider; foreign type Session: void* invalid null; "
          "foreign fn open() -> owned Session where result obliges close; "
          "foreign fn wait(session: ref_mut Session) -> void where session "
          "invokes complete;"),
      chtholly::compiler::CompilationUnitKind::ForeignBinding);
  const auto consumer = session.addUnit(
      SourceInput("consumer.cfdl",
                  "module consumer; import provider; "
                  "foreign fn close(session: ref_mut Session) -> void "
                  "where session discharges provider::close;"),
      chtholly::compiler::CompilationUnitKind::ForeignBinding);
  CHTHOLLY_TEST_CHECK(provider.hasValue() && consumer.hasValue());
  std::string error;
  if (!session.compile(error)) {
    std::cerr << "imported CFDL nominal compile failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(!session.unit(consumer).printLLVM().empty());
}

void imported_cfdl_nominal_name_ambiguity_rejected() {
  CompilationSession session("x86_64-pc-windows-msvc", "nominal-ambiguity");
  CHTHOLLY_TEST_CHECK(session
             .addUnit(
                 SourceInput("left.cfdl", "module left; foreign type Session;"),
                 chtholly::compiler::CompilationUnitKind::ForeignBinding)
             .hasValue());
  CHTHOLLY_TEST_CHECK(session
             .addUnit(SourceInput("right.cfdl",
                                  "module right; foreign type Session;"),
                      chtholly::compiler::CompilationUnitKind::ForeignBinding)
             .hasValue());
  CHTHOLLY_TEST_CHECK(
      session
          .addUnit(SourceInput(
                       "consumer.cfdl",
                       "module consumer; import left; import right; "
                       "foreign fn observe(session: ref_mut Session) -> i32;"),
                   chtholly::compiler::CompilationUnitKind::ForeignBinding)
          .hasValue());
  std::string error;
  CHTHOLLY_TEST_CHECK(!session.compile(error));
  CHTHOLLY_TEST_CHECK(error.find("duplicate") != std::string::npos ||
         error.find("unknown CFDL ABI type") != std::string::npos);
}

void cfdl_provider_initializes_cross_package_places() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path() /
                    ("chtholly-token-provider-" + std::to_string(nonce));
  fs::create_directories(root);
  const auto sidecar = root / "token-provider.interop";
  const auto provider_source = fixtureText(
      fs::path("chtholly-cfdl-token-provider") / "provider" / "token.cfdl");

  CompilationSession provider("x86_64-pc-windows-msvc", "token-provider");
  const auto provider_unit =
      provider.addUnit(SourceInput("token.cfdl", provider_source),
                       chtholly::compiler::CompilationUnitKind::ForeignBinding);
  CHTHOLLY_TEST_CHECK(provider_unit.hasValue());
  std::string error;
  if (!provider.compile(error)) {
    std::cerr << "token provider compile failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(provider.exportInteropBundle(sidecar.string(), error));
  ArtifactBundle provider_bundle;
  CHTHOLLY_TEST_CHECK(chtholly::compiler::interop::readArtifactBundle(sidecar.string(),
                                                     provider_bundle, error));
  const auto ticket_record =
      std::ranges::find_if(provider_bundle.records, [](const auto &record) {
        return record.reference.canonical_name == "acquire_ticket";
      });
  CHTHOLLY_TEST_CHECK(ticket_record != provider_bundle.records.end());
  CHTHOLLY_TEST_CHECK(ticket_record->artifact.out_lanes == std::vector<std::uint32_t>{0});
  CHTHOLLY_TEST_CHECK(ticket_record->artifact.status_lane ==
         chtholly::core::AnyId::InvalidIndex);

  const auto *provider_module =
      provider.packageManifest().findModule("token_provider");
  CHTHOLLY_TEST_CHECK(provider_module != nullptr);
  const auto *ticket_function =
      provider_module->public_interface.findFunction("acquire_ticket");
  CHTHOLLY_TEST_CHECK(ticket_function != nullptr);
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      ticket_function->ownership_summary.effects, [](const auto &effect) {
        return effect.kind == chtholly::compiler::CallableEffectKind::Initialize &&
               effect.region.parameter_index == 0 && effect.region.path.empty();
      }));

  CompilationSession wrappers("x86_64-pc-windows-msvc", "token-wrappers");
  CHTHOLLY_TEST_CHECK(wrappers.loadInteropBundle(sidecar.string(), "token-provider", error));
  CHTHOLLY_TEST_CHECK(
      wrappers
          .addUnit(SourceInput(
              "wrappers.cns", fixtureText(fs::path("chtholly-cfdl-token-provider") /
                                          "provider" / "wrappers.cns")))
          .hasValue());
  chtholly::compiler::CompilationRequest wrappers_request;
  wrappers_request.dependency_manifests = {&provider.packageManifest()};
  if (!wrappers.compile(error, wrappers_request)) {
    std::cerr << "generic token wrapper compile failed: " << error << "\n";
    std::abort();
  }
  CHTHOLLY_TEST_CHECK(wrappers.packageManifest().findModule("token_wrappers") != nullptr);

  CompilationSession consumer("x86_64-pc-windows-msvc", "token-consumer");
  CHTHOLLY_TEST_CHECK(consumer.loadInteropBundle(sidecar.string(), "token-provider", error));
  const auto consumer_unit = consumer.addUnit(
      SourceInput("main.cns", fixtureText(fs::path("chtholly-cfdl-token-provider") /
                                          "consumer" / "main.cns")));
  chtholly::compiler::CompilationRequest request;
  request.dependency_manifests = {&provider.packageManifest(),
                                  &wrappers.packageManifest()};
  if (!consumer.compile(error, request)) {
    std::cerr << "token consumer compile failed: " << error << "\n";
    std::abort();
  }
  const auto llvm = consumer.unit(consumer_unit).printLLVM();
  CHTHOLLY_TEST_CHECK(llvm.find("@acquire") != std::string::npos);
  CHTHOLLY_TEST_CHECK(llvm.find("@release") != std::string::npos);
  CHTHOLLY_TEST_CHECK(llvm.find("@acquire_ticket") != std::string::npos);
  CHTHOLLY_TEST_CHECK(llvm.find("@release_ticket") != std::string::npos);

  const auto *consumer_sem_ir = consumer.unit(consumer_unit).semIR();
  CHTHOLLY_TEST_CHECK(consumer_sem_ir != nullptr);
  const auto components =
      std::vector<chtholly::compiler::ConcreteSpecializationComponentArtifact>(
          consumer_sem_ir->specializationComponents().begin(),
          consumer_sem_ir->specializationComponents().end());
  CHTHOLLY_TEST_CHECK(!components.empty());
  const chtholly::compiler::ConcreteSpecificNodeArtifact *wrapper_node = nullptr;
  const chtholly::compiler::ConcreteSpecializationComponentArtifact
      *wrapper_component = nullptr;
  for (const auto &component : components)
    for (const auto &node : component.nodes())
      if (node.template_entity.canonical_name == "acquire_through_wrapper") {
        wrapper_node = &node;
        wrapper_component = &component;
      }
  CHTHOLLY_TEST_CHECK(wrapper_node != nullptr && wrapper_component != nullptr);
  CHTHOLLY_TEST_CHECK(!wrapper_node->callees.empty());
  auto signature_tampered_nodes =
      std::vector<chtholly::compiler::ConcreteSpecificNodeArtifact>(
          wrapper_component->nodes().begin(), wrapper_component->nodes().end());
  const auto signature_tampered_node = std::ranges::find_if(
      signature_tampered_nodes, [&](const auto &candidate) {
        return candidate.request_fingerprint ==
               wrapper_node->request_fingerprint;
      });
  CHTHOLLY_TEST_CHECK(signature_tampered_node != signature_tampered_nodes.end());
  auto &tampered_callable =
      signature_tampered_node->callees.front().callable_type;
  CHTHOLLY_TEST_CHECK(tampered_callable.kind == chtholly::compiler::PublicTypeKind::Function &&
         !tampered_callable.arguments.empty());
  tampered_callable.arguments.back() = chtholly::compiler::PublicTypeKind::Bool;
  chtholly::compiler::ConcreteSpecializationComponentArtifact
      signature_tampered_component(
          wrapper_component->semanticOptionsFingerprint(),
          std::move(signature_tampered_nodes));
  std::string signature_error;
  CHTHOLLY_TEST_CHECK(!signature_tampered_component.verify(signature_error));
  auto signature_tampered_components = components;
  const auto original_wrapper_fingerprint = wrapper_component->fingerprint();
  for (auto &component : signature_tampered_components)
    if (component.fingerprint() == original_wrapper_fingerprint)
      component = signature_tampered_component;
  const auto initializes_parameter = [](const auto &summary) {
    return std::ranges::any_of(
               summary.effects,
               [](const auto &effect) {
                 return effect.kind ==
                            chtholly::compiler::CallableEffectKind::Initialize &&
                        effect.region.parameter_index == 0 &&
                        effect.region.path.empty();
               }) &&
           std::ranges::any_of(
               summary.postconditions, [](const auto &postcondition) {
                 return postcondition.region.parameter_index == 0 &&
                        postcondition.region.path.empty() &&
                        postcondition.outcomes ==
                            chtholly::compiler::CallableOutcomeInitialize;
               });
  };
  CHTHOLLY_TEST_CHECK(initializes_parameter(wrapper_node->ownership_summary));
  CHTHOLLY_TEST_CHECK(wrapper_node->arguments.size() == 1);
  CHTHOLLY_TEST_CHECK(wrapper_node->request_fingerprint ==
         chtholly::compiler::fingerprintConcreteSpecializationRequest(
             wrapper_node->template_entity, wrapper_node->arguments,
             wrapper_node->constraint_witnesses,
             wrapper_component->semanticOptionsFingerprint()));
  auto changed_entity = wrapper_node->template_entity;
  changed_entity.expected_fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes(
          "generic-wrapper-with-changed-initialization-summary");
  CHTHOLLY_TEST_CHECK(wrapper_node->request_fingerprint !=
         chtholly::compiler::fingerprintConcreteSpecializationRequest(
             changed_entity, wrapper_node->arguments,
             wrapper_node->constraint_witnesses,
             wrapper_component->semanticOptionsFingerprint()));

  CompilationSession warm_consumer("x86_64-pc-windows-msvc", "token-consumer");
  CHTHOLLY_TEST_CHECK(warm_consumer.loadInteropBundle(sidecar.string(), "token-provider",
                                         error));
  const auto warm_unit = warm_consumer.addUnit(
      SourceInput("main.cns", fixtureText(fs::path("chtholly-cfdl-token-provider") /
                                          "consumer" / "main.cns")));
  auto warm_request = request;
  warm_request.load_specialization =
      [components](const chtholly::compiler::StableFingerprint &fingerprint) {
        const auto found =
            std::ranges::find_if(components, [&](const auto &component) {
              return component.findNode(fingerprint) != nullptr;
            });
        return chtholly::compiler::ConcreteSpecializationLoadResult{
            found == components.end()
                ? chtholly::compiler::ConcreteSpecializationLoadStatus::Missing
                : chtholly::compiler::ConcreteSpecializationLoadStatus::Found,
            found == components.end()
                ? std::vector<
                      chtholly::compiler::ConcreteSpecializationComponentArtifact>{}
                : components,
            {}};
      };
  CHTHOLLY_TEST_CHECK(warm_consumer.compile(error, warm_request));
  const auto *warm_sem_ir = warm_consumer.unit(warm_unit).semIR();
  CHTHOLLY_TEST_CHECK(warm_sem_ir != nullptr);
  CHTHOLLY_TEST_CHECK(warm_sem_ir->specializationCacheStats().hits > 0);
  CHTHOLLY_TEST_CHECK(warm_sem_ir->specializationCacheStats().rebuilt_components == 0);

  CompilationSession repaired_consumer("x86_64-pc-windows-msvc",
                                       "token-consumer");
  CHTHOLLY_TEST_CHECK(repaired_consumer.loadInteropBundle(sidecar.string(), "token-provider",
                                             error));
  const auto repaired_unit = repaired_consumer.addUnit(
      SourceInput("main.cns", fixtureText(fs::path("chtholly-cfdl-token-provider") /
                                          "consumer" / "main.cns")));
  auto repaired_request = request;
  repaired_request.load_specialization =
      [signature_tampered_components](
          const chtholly::compiler::StableFingerprint &fingerprint) {
        const auto found = std::ranges::find_if(
            signature_tampered_components, [&](const auto &component) {
              return component.findNode(fingerprint) != nullptr;
            });
        return chtholly::compiler::ConcreteSpecializationLoadResult{
            found == signature_tampered_components.end()
                ? chtholly::compiler::ConcreteSpecializationLoadStatus::Missing
                : chtholly::compiler::ConcreteSpecializationLoadStatus::Found,
            found == signature_tampered_components.end()
                ? std::vector<
                      chtholly::compiler::ConcreteSpecializationComponentArtifact>{}
                : signature_tampered_components,
            {}};
      };
  CHTHOLLY_TEST_CHECK(repaired_consumer.compile(error, repaired_request));
  const auto *repaired_sem_ir = repaired_consumer.unit(repaired_unit).semIR();
  CHTHOLLY_TEST_CHECK(repaired_sem_ir != nullptr);
  CHTHOLLY_TEST_CHECK(repaired_sem_ir->specializationCacheStats().semantic_rejections > 0);
  CHTHOLLY_TEST_CHECK(repaired_sem_ir->specializationCacheStats().rebuilt_components > 0);

  auto tampered_components = components;
  const auto wrapper_component_fingerprint = wrapper_component->fingerprint();
  const auto wrapper_request_fingerprint = wrapper_node->request_fingerprint;
  for (auto &component : tampered_components) {
    if (component.fingerprint() != wrapper_component_fingerprint)
      continue;
    auto nodes = std::vector<chtholly::compiler::ConcreteSpecificNodeArtifact>(
        component.nodes().begin(), component.nodes().end());
    const auto node = std::ranges::find_if(nodes, [&](const auto &candidate) {
      return candidate.request_fingerprint == wrapper_request_fingerprint;
    });
    CHTHOLLY_TEST_CHECK(node != nodes.end());
    std::erase_if(node->ownership_summary.effects, [](const auto &effect) {
      return effect.kind == chtholly::compiler::CallableEffectKind::Initialize &&
             effect.region.parameter_index == 0;
    });
    std::erase_if(node->ownership_summary.postconditions,
                  [](const auto &postcondition) {
                    return postcondition.region.parameter_index == 0;
                  });
    component = chtholly::compiler::ConcreteSpecializationComponentArtifact(
        component.semanticOptionsFingerprint(), std::move(nodes));
    std::string verify_error;
    CHTHOLLY_TEST_CHECK(component.verify(verify_error));
    CHTHOLLY_TEST_CHECK(component.fingerprint() != wrapper_component_fingerprint);
  }
  CompilationSession tampered_consumer("x86_64-pc-windows-msvc",
                                       "token-consumer");
  CHTHOLLY_TEST_CHECK(tampered_consumer.loadInteropBundle(sidecar.string(), "token-provider",
                                             error));
  CHTHOLLY_TEST_CHECK(tampered_consumer
             .addUnit(SourceInput(
                 "main.cns", fixtureText(fs::path("chtholly-cfdl-token-provider") /
                                         "consumer" / "main.cns")))
             .hasValue());
  auto tampered_request = request;
  tampered_request.load_specialization =
      [tampered_components](
          const chtholly::compiler::StableFingerprint &fingerprint) {
        const auto found = std::ranges::find_if(
            tampered_components, [&](const auto &component) {
              return component.findNode(fingerprint) != nullptr;
            });
        return chtholly::compiler::ConcreteSpecializationLoadResult{
            found == tampered_components.end()
                ? chtholly::compiler::ConcreteSpecializationLoadStatus::Missing
                : chtholly::compiler::ConcreteSpecializationLoadStatus::Found,
            found == tampered_components.end()
                ? std::vector<
                      chtholly::compiler::ConcreteSpecializationComponentArtifact>{}
                : tampered_components,
            {}};
      };
  error.clear();
  CHTHOLLY_TEST_CHECK(!tampered_consumer.compile(error, tampered_request));
  CHTHOLLY_TEST_CHECK(error.find("chtholly.next.sem.ownership.artifact-mismatch") !=
         std::string::npos);

  const std::vector<std::pair<fs::path, std::string_view>> negative = {
      {"read-before-initialize.cns",
       "chtholly.next.sem.place.uninitialized-storage"},
      {"move-before-initialize.cns",
       "chtholly.next.sem.place.uninitialized-storage"},
      {"scope-exit-before-initialize.cns",
       "chtholly.next.sem.place.uninitialized-storage"},
      {"forwarding-without-initialize.cns",
       "chtholly.next.sem.place.uninitialized-storage"},
      {"forwarding-read-before-initialize.cns",
       "chtholly.next.sem.place.use-after-move"},
      {"initialize-twice.cns", "chtholly.next.sem.assign.invalid-place"},
  };
  for (const auto &[file, expected_diagnostic] : negative) {
    CompilationSession rejected("x86_64-pc-windows-msvc",
                                "token-negative-" + file.stem().string());
    CHTHOLLY_TEST_CHECK(
        rejected.loadInteropBundle(sidecar.string(), "token-provider", error));
    CHTHOLLY_TEST_CHECK(rejected
               .addUnit(SourceInput(
                   file.string(),
                   fixtureText(fs::path("chtholly-cfdl-token-provider") /
                               "negative" / file)))
               .hasValue());
    error.clear();
    if (rejected.compile(error, request)) {
      std::cerr << "negative initialization fixture unexpectedly compiled: "
                << file.string() << "\n";
      std::abort();
    }
    if (error.find(expected_diagnostic) == std::string::npos) {
      std::cerr << "negative initialization fixture '" << file.string()
                << "' produced an unexpected diagnostic: " << error << "\n";
      std::abort();
    }
  }

  std::error_code cleanup_error;
  fs::remove_all(root, cleanup_error);
}

void chtholly_contract_provider_consumes_cross_package_summary() {
  CompilationSession provider("x86_64-pc-windows-msvc", "contract-provider");
  const auto provider_unit =
      provider.addUnit(SourceInput("provider.cns",
                                   R"cns(module contract_provider;
pub struct Box { pub value: i32; }

pub fn make(): Box {
  return Box { .value = 7 };
}

pub fn borrow(source: const Box&): const Box& contract {
  borrows shared source;
  returns borrow source;
}

pub fn update(target: Box&): void contract {
  writes target;
  ensures initialized target;
}
)cns"));
  CHTHOLLY_TEST_CHECK(provider_unit.hasValue());
  std::string error;
  if (!provider.compile(error)) {
    std::cerr << "Chtholly contract provider compile failed: " << error << "\n";
    std::abort();
  }
  const auto *provider_module =
      provider.packageManifest().findModule("contract_provider");
  CHTHOLLY_TEST_CHECK(provider_module != nullptr);
  const auto *borrow_function =
      provider_module->public_interface.findFunction("borrow");
  const auto *update_function =
      provider_module->public_interface.findFunction("update");
  CHTHOLLY_TEST_CHECK(borrow_function != nullptr && update_function != nullptr);
  CHTHOLLY_TEST_CHECK(borrow_function->declaration_kind ==
         chtholly::compiler::PublicCallableDeclarationKind::Forward);
  CHTHOLLY_TEST_CHECK(update_function->declaration_kind ==
         chtholly::compiler::PublicCallableDeclarationKind::Forward);
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      borrow_function->ownership_summary.effects, [](const auto &effect) {
        return effect.kind ==
                   chtholly::compiler::CallableEffectKind::BorrowShared &&
               effect.region.parameter_index == 0;
      }));
  CHTHOLLY_TEST_CHECK(!borrow_function->ownership_summary.return_provenance.empty());
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      update_function->ownership_summary.effects, [](const auto &effect) {
        return effect.kind == chtholly::compiler::CallableEffectKind::Write &&
               effect.region.parameter_index == 0;
      }));
  CHTHOLLY_TEST_CHECK(std::ranges::any_of(
      update_function->ownership_summary.postconditions,
      [](const auto &postcondition) {
        return postcondition.region.parameter_index == 0 &&
               (postcondition.outcomes &
                chtholly::compiler::CallableOutcomeInitialize) != 0;
      }));

  CompilationSession consumer("x86_64-pc-windows-msvc", "contract-consumer");
  const auto consumer_unit =
      consumer.addUnit(SourceInput("consumer.cns",
                                   R"cns(module contract_consumer;
import contract_provider;

fn main(): i32 {
  let box = contract_provider::make();
  let view = contract_provider::borrow(&box);
  return view.value;
}
)cns"));
  CHTHOLLY_TEST_CHECK(consumer_unit.hasValue());
  chtholly::compiler::CompilationRequest request;
  request.dependency_manifests = {&provider.packageManifest()};
  if (!consumer.compile(error, request)) {
    std::cerr << "Chtholly contract consumer compile failed: " << error << "\n";
    std::abort();
  }
  const auto llvm = consumer.unit(consumer_unit).printLLVM();
  CHTHOLLY_TEST_CHECK(llvm.find("declare") != std::string::npos);
}

void runtime_symbol_mapping_configuration_rejects_aliases() {
  CompilationSession session("x86_64-pc-windows-msvc", "runtime-mappings");
  std::string error;
  CHTHOLLY_TEST_CHECK(!session.setRuntimeSymbolMappings(
      {{"first", "runtime"}, {"second", "runtime"}}, error));
  CHTHOLLY_TEST_CHECK(error.find("unique") != std::string::npos);
}

void cfdl_foreign_carrier_rules_are_closed() {
  std::string error;
  {
    CompilationSession session("x86_64-pc-windows-msvc", "incomplete-ref");
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(SourceInput("binding.cfdl",
                                 "module binding; foreign type FILE; "
                                 "foreign fn inspect(file: ref FILE) -> i32;"),
                     chtholly::compiler::CompilationUnitKind::ForeignBinding)
            .hasValue());
    CHTHOLLY_TEST_CHECK(session.compile(error));
  }
  for (const auto source : {
           "module bad; foreign type FILE; foreign fn take(file: FILE) -> i32;",
           "module bad; foreign type FILE; foreign fn fill(file: out FILE) -> "
           "i32;",
           "module bad; foreign type Handle: i32 invalid null; foreign "
           "fn use(handle: Handle) -> i32;",
           "module bad; foreign type Handle: i8 invalid 128; foreign fn "
           "use(handle: Handle) -> i32;",
           "module bad; foreign type Handle: u8 invalid -1; foreign fn "
           "use(handle: Handle) -> i32;",
           "module bad; foreign type Handle: const void*; foreign fn "
           "use(handle: Handle) -> i32;",
           "module bad; foreign struct Loop { child: Loop; }; foreign fn "
           "use(value: Loop) -> i32;",
           "module bad; foreign union Empty {}; foreign fn use(value: "
           "Empty) -> i32;",
           "module bad; foreign enum E: i8 { TOO_BIG = 128; }; foreign fn "
           "use(value: E) -> i32;",
           "module bad; foreign const HIGH: c_int = 6; foreign const LOW: "
           "c_int = 4; foreign fn f() -> c_int error code when result "
           "in { HIGH through LOW };",
           "module bad; foreign type Handle: void*; foreign fn f() -> Handle "
           "error win32 when result == invalid;",
           "module bad; foreign fn f() -> void* error win32 when result "
           "== invalid;",
           "module bad; foreign fn wrong() -> i32 call sysv64;",
           "module bad; foreign fn callback(value: c_fn sysv64(i32)->void) -> "
           "i32;",
       }) {
    CompilationSession session("x86_64-pc-windows-msvc", "bad-carrier");
    CHTHOLLY_TEST_CHECK(session
               .addUnit(SourceInput("bad.cfdl", source),
                        chtholly::compiler::CompilationUnitKind::ForeignBinding)
               .hasValue());
    error.clear();
    CHTHOLLY_TEST_CHECK(!session.compile(error));
    CHTHOLLY_TEST_CHECK(!error.empty());
  }
  {
    CompilationSession session("x86_64-pc-windows-msvc", "hidden-carrier");
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(SourceInput("binding.cfdl",
                                 "module binding; foreign struct Pair { left: "
                                 "i32; right: i32; }; "
                                 "foreign fn make(value: out Pair) -> i32;"),
                     chtholly::compiler::CompilationUnitKind::ForeignBinding)
            .hasValue());
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(SourceInput(
                "main.cns",
                "module main; import binding as api; fn main(): i32 { unsafe { "
                "let pair: api::Pair; if (api::make(pair) != 0) { return -1; } "
                "return pair.left; } }"))
            .hasValue());
    error.clear();
    CHTHOLLY_TEST_CHECK(!session.compile(error));
    CHTHOLLY_TEST_CHECK(!error.empty());
  }
  {
    CompilationSession session("x86_64-pc-windows-msvc", "carrier-identity");
    CHTHOLLY_TEST_CHECK(session
               .addUnit(SourceInput(
                            "binding.cfdl",
                            "module binding; "
                            "foreign struct Left { value: i32; }; "
                            "foreign struct Right { value: i32; }; "
                            "foreign fn make_left(value: out Left) -> i32; "
                            "foreign fn consume_right(value: Right) -> i32;"),
                        chtholly::compiler::CompilationUnitKind::ForeignBinding)
               .hasValue());
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(SourceInput(
                "main.cns",
                "module main; import binding as api; fn main(): i32 { unsafe { "
                "let value: api::Left; if (api::make_left(value) != 0) { "
                "return -1; } "
                "return api::consume_right(value); } }"))
            .hasValue());
    error.clear();
    CHTHOLLY_TEST_CHECK(!session.compile(error));
  }
  {
    CompilationSession session("x86_64-pc-windows-msvc", "nested-carrier");
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(
                SourceInput(
                    "provider.cfdl",
                    "module provider; foreign struct Inner { value: i32; };"),
                chtholly::compiler::CompilationUnitKind::ForeignBinding)
            .hasValue());
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(SourceInput(
                         "consumer.cfdl",
                         "module consumer; import provider; "
                         "foreign struct Outer { inner: Inner; marker: i32; }; "
                         "foreign fn consume(value: Outer) -> i32;"),
                     chtholly::compiler::CompilationUnitKind::ForeignBinding)
            .hasValue());
    error.clear();
    CHTHOLLY_TEST_CHECK(session.compile(error));
  }
  {
    CompilationSession session("x86_64-pc-windows-msvc", "symbol-conflict");
    CHTHOLLY_TEST_CHECK(session
               .addUnit(SourceInput(
                            "left.cfdl",
                            "module left; foreign fn left(value: i32) -> i32 "
                            "link \"shared_symbol\";"),
                        chtholly::compiler::CompilationUnitKind::ForeignBinding)
               .hasValue());
    CHTHOLLY_TEST_CHECK(session
               .addUnit(SourceInput(
                            "right.cfdl",
                            "module right; foreign fn right(value: i64) -> i32 "
                            "link \"shared_symbol\";"),
                        chtholly::compiler::CompilationUnitKind::ForeignBinding)
               .hasValue());
    error.clear();
    CHTHOLLY_TEST_CHECK(!session.compile(error));
    CHTHOLLY_TEST_CHECK(error.find("conflicting foreign symbol") != std::string::npos);
  }
}

void cfdl_epoch9_native_provider_executes() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path() /
                    ("chtholly-cfdl-epoch9-" + std::to_string(nonce));
  fs::create_directories(root);
  writeBinary(root / "binding.cfdl",
              "module binding;\n"
              "foreign enum Status: c_int { STATUS_OK = 0; "
              "STATUS_FAILED = -1; };\n"
              "foreign union Number { integer: c_long; real: f64; };\n"
              "foreign fn make(value: out Number) -> Status "
              "link \"c_number_make\" call c;\n"
              "foreign fn sum(value: Number) -> i32 "
              "link \"c_number_sum\" call c;\n"
              "foreign fn status_is_ok(value: Status) -> i32 "
              "link \"c_status_is_ok\" call c;\n");
  writeBinary(root / "main.cns",
              "module main; import binding as api; fn main(): i32 { unsafe { "
              "let value: api::Number; api::make(value); "
              "if (api::status_is_ok(api::STATUS_OK) != 1) { return 2; } "
              "if (api::sum(value) != 42) { return 1; } return 0; } }\n");
  auto provider = fs::path(CHTHOLLY_CFDL_EPOCH9_PROVIDER).generic_string();
  writeBinary(root / "chtholly.toml",
              "[package]\nname = \"cfdl-epoch9-native\"\n"
              "language = \"1.9\"\n"
              "[build]\nentry = \"main.cns\"\nmodule_paths = [\".\"]\n"
              "[native]\nlink_libraries = [\"" +
                  provider + "\"]\n");
  chtholly::CompilerInvocation invocation;
  invocation.workflow = chtholly::DriverWorkflow::Run;
  invocation.project_path = root.string();
  invocation.out_dir = (root / "bin").string();
  invocation.cache_dir = (root / "cache").string();
#ifdef _WIN32
  invocation.target_triple = "x86_64-pc-windows-msvc";
#else
  invocation.target_triple = "x86_64-unknown-linux-gnu";
#endif
  invocation.resource_dir =
      (fs::path(CHTHOLLY_BINARY_DIR) / "share" / "chtholly").string();
  invocation.suppress_lockfile_update = true;
  std::string error;
  if (chtholly::runCompilerPipeline(invocation, error) != 0) {
    std::cerr << "CFDL epoch-9 native provider failed: " << error << "\n";
    std::abort();
  }
  std::error_code cleanup_error;
  fs::remove_all(root, cleanup_error);
}

void cffi_identity_enters_internal_artifacts() {
  auto first_identity = testCFFIIdentity();
  auto second_identity = first_identity;
  second_identity.facts = chtholly::sha256Hex("different-facts");
  const auto compile = [](const chtholly::compiler::CFFIReceiptIdentity &identity) {
    CompilationSession session(
        identity.target, "cffi-identity", {},
        chtholly::compiler::defaultCompileToolchainFingerprint(), {},
        chtholly::CurrentLanguageContract, {}, identity);
    CHTHOLLY_TEST_CHECK(
        session
            .addUnit(SourceInput("generated.cfdl",
                                 "module generated; foreign fn identity(value: "
                                 "i32) -> i32 link \"c_add_one\" call c;"),
                     chtholly::compiler::CompilationUnitKind::ForeignBinding)
            .hasValue());
    std::string error;
    CHTHOLLY_TEST_CHECK(session.compile(error));
    return session;
  };
  auto first = compile(first_identity);
  auto second = compile(second_identity);
  CHTHOLLY_TEST_CHECK(first.packageCheckArtifact().fingerprint() !=
         second.packageCheckArtifact().fingerprint());
  CHTHOLLY_TEST_CHECK(first.packageManifest().configurationFingerprint() !=
         second.packageManifest().configurationFingerprint());
  CHTHOLLY_TEST_CHECK(first.packageManifest().fingerprint() !=
         second.packageManifest().fingerprint());

  std::string error;
  const auto bytes = first.packageManifest().encode(error);
  CHTHOLLY_TEST_CHECK(error.empty());
  auto decoded =
      chtholly::compiler::CompilerPackageArtifactManifest::decode(bytes, error);
  CHTHOLLY_TEST_CHECK(decoded.has_value());
  CHTHOLLY_TEST_CHECK(decoded->cffiIdentity() == first.packageManifest().cffiIdentity());
}

void cffi_receipt_parser_is_strict() {
  auto identity = testCFFIIdentity();
  std::string error;
  const auto receipt = chtholly::compiler::renderCFFIReceipt(identity, error);
  CHTHOLLY_TEST_CHECK(!receipt.empty());
  CHTHOLLY_TEST_CHECK(chtholly::compiler::parseCFFIReceipt(receipt, error).has_value());

  const auto rejects = [&](std::string text) {
    error.clear();
    CHTHOLLY_TEST_CHECK(!chtholly::compiler::parseCFFIReceipt(text, error));
    CHTHOLLY_TEST_CHECK(!error.empty());
  };
  rejects("CHCFFI1\ntarget\tx86_64-pc-windows-msvc\n");
  auto unknown = receipt;
  unknown.replace(unknown.find("facts\t"), 5, "unknown");
  rejects(std::move(unknown));
  auto uppercase = receipt;
  const auto clang_digest = uppercase.find("clang-version\t") + 14;
  uppercase[clang_digest] = 'A';
  rejects(std::move(uppercase));
  auto crlf = receipt;
  crlf.replace(crlf.find('\n'), 1, "\r\n");
  rejects(std::move(crlf));
  rejects(receipt + "trailing\n");
  rejects(std::string(20U * 1024U, 'x'));

  identity.target = std::string("x86_64\0bad", 10);
  CHTHOLLY_TEST_CHECK(chtholly::compiler::renderCFFIReceipt(identity, error).empty());
}

} // namespace

int main() {
#define RUN_STAGE(name, function) \
  do { std::cerr << "[package-artifact] " << name << "\n"; function(); } while (false)
  RUN_STAGE("manifest", manifest_and_archive_round_trip);
  RUN_STAGE("session", session_loads_before_compile);
  RUN_STAGE("tamper", tampered_and_mismatched_bundles_are_rejected);
  RUN_STAGE("plan", next_plan_resolves_declared_bundle);
  RUN_STAGE("archive-compile", archive_dependency_enters_real_compile_session);
  RUN_STAGE("stdlib-sidecar", stdlib_provider_generates_real_sidecar);
  RUN_STAGE("store-race", artifact_store_install_race);
  RUN_STAGE("cfdl-llvm", cfdl_source_to_import_and_llvm);
  RUN_STAGE("stdlib-cfdl", stdlib_cfdl_fixture_compiles);
  RUN_STAGE("stdlib-host-cfdl", stdlib_host_cfdl_fixture_compiles);
  RUN_STAGE("qualified-cfdl", qualified_cfdl_protocol_reference_resolves);
  RUN_STAGE("foreign-nominal", imported_cfdl_nominal_type_materializes);
  RUN_STAGE("nominal-ambiguity", imported_cfdl_nominal_name_ambiguity_rejected);
  RUN_STAGE("place-init", cfdl_provider_initializes_cross_package_places);
  RUN_STAGE("contract-summary", chtholly_contract_provider_consumes_cross_package_summary);
  RUN_STAGE("runtime-symbols", runtime_symbol_mapping_configuration_rejects_aliases);
  RUN_STAGE("carrier-rules", cfdl_foreign_carrier_rules_are_closed);
  RUN_STAGE("cffi-artifacts", cffi_identity_enters_internal_artifacts);
  RUN_STAGE("cffi-receipt", cffi_receipt_parser_is_strict);
  RUN_STAGE("native-provider", cfdl_epoch9_native_provider_executes);
  RUN_STAGE("host-e2e", stdlib_host_generated_executable_runs_provider_symbols);
#undef RUN_STAGE
  return 0;
}
